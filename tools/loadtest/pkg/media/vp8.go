package media

import (
	"context"
	"math/rand"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

const (
	VP8ClockRate          = 90000
	VP8DefaultPayloadType = 96
	VP8MaxPayloadChunk    = 1150 // bytes per RTP packet to avoid MTU fragmentation
)

// VP8Generator generates synthetic paced VP8 video frames.
type VP8Generator struct {
	track       *webrtc.TrackLocalStaticRTP
	payloadType uint8
	ssrc        uint32
	fps         int
	bitrateBps  int

	seq       uint16
	timestamp uint32
	frameIdx  uint64

	keyframeNeeded atomic.Bool
	packetsSent    atomic.Uint64
	bytesSent      atomic.Uint64

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// NewVP8Generator creates a new VP8 frame generator.
func NewVP8Generator(track *webrtc.TrackLocalStaticRTP, payloadType uint8, ssrc uint32, fps, bitrateBps int) *VP8Generator {
	if payloadType == 0 {
		payloadType = VP8DefaultPayloadType
	}
	if ssrc == 0 {
		ssrc = rand.Uint32()
	}
	if fps <= 0 {
		fps = 30
	}
	if bitrateBps <= 0 {
		bitrateBps = 500000 // 500 kbps default
	}

	gen := &VP8Generator{
		track:       track,
		payloadType: payloadType,
		ssrc:        ssrc,
		fps:         fps,
		bitrateBps:  bitrateBps,
		seq:         uint16(rand.Intn(65535)),
		timestamp:   rand.Uint32(),
	}
	gen.keyframeNeeded.Store(true) // Start with keyframe
	return gen
}

// RequestKeyframe marks that the next frame should be a keyframe (e.g. after PLI/FIR).
func (g *VP8Generator) RequestKeyframe() {
	g.keyframeNeeded.Store(true)
}

// buildVP8Frame constructs a synthetic VP8 bitstream with proper VP8 headers.
func (g *VP8Generator) buildVP8Frame(isKeyframe bool, targetSizeBytes int) []byte {
	if isKeyframe {
		// Keyframe Header (RFC 6386 section 9.1):
		// Uncompressed data chunk: 3 bytes
		// Tag: Bit 0 = 0 (keyframe), Bits 1-3 = 0 (version), Bit 4 = 1 (show_frame), Bits 5-23 = partition_size
		// Sync code: 0x9d 0x01 0x2a
		// Dimensions: Width 640 (0x0280), Height 360 (0x0168)
		hdr := []byte{
			0x10, 0x02, 0x00, // 3-byte uncompressed header
			0x9d, 0x01, 0x2a, // 3-byte VP8 start code
			0x80, 0x02, // Width = 640 (little endian + 0 scale)
			0x68, 0x01, // Height = 360 (little endian + 0 scale)
		}
		if targetSizeBytes < len(hdr)+32 {
			targetSizeBytes = len(hdr) + 32
		}
		frame := make([]byte, targetSizeBytes)
		copy(frame, hdr)
		return frame
	}

	// Interframe Header:
	// Bit 0 = 1 (interframe), Bit 4 = 1 (show_frame) -> 0x11
	hdr := []byte{0x11, 0x00, 0x00}
	if targetSizeBytes < len(hdr)+16 {
		targetSizeBytes = len(hdr) + 16
	}
	frame := make([]byte, targetSizeBytes)
	copy(frame, hdr)
	return frame
}

// GenerateFramePackets creates the RTP packets for one video frame.
func (g *VP8Generator) GenerateFramePackets() []*rtp.Packet {
	isKeyframe := g.keyframeNeeded.Swap(false)
	if g.frameIdx%uint64(g.fps*2) == 0 { // Periodic keyframe every 2 seconds
		isKeyframe = true
	}

	// Calculate target frame size based on bitrate and fps
	bytesPerSec := g.bitrateBps / 8
	bytesPerFrame := bytesPerSec / g.fps
	if isKeyframe {
		bytesPerFrame = int(float64(bytesPerFrame) * 1.8) // Keyframes are slightly larger
	}
	if bytesPerFrame < 128 {
		bytesPerFrame = 128
	}

	rawFrame := g.buildVP8Frame(isKeyframe, bytesPerFrame)

	// Slice rawFrame into MTU chunks with RFC 7741 VP8 payload descriptors
	var packets []*rtp.Packet
	remaining := rawFrame

	for len(remaining) > 0 {
		chunkSize := len(remaining)
		if chunkSize > VP8MaxPayloadChunk {
			chunkSize = VP8MaxPayloadChunk
		}

		chunk := remaining[:chunkSize]
		remaining = remaining[chunkSize:]

		isStart := len(packets) == 0
		isEnd := len(remaining) == 0

		// RFC 7741 section 4.1: VP8 Payload Descriptor
		// Byte 0: [X R N S R PID PID PID]
		// S=1 on start of frame/partition (0x10), S=0 otherwise (0x00)
		var descriptor byte
		if isStart {
			descriptor = 0x10
		} else {
			descriptor = 0x00
		}

		payload := make([]byte, 1+len(chunk))
		payload[0] = descriptor
		copy(payload[1:], chunk)

		pkt := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				PayloadType:    g.payloadType,
				SequenceNumber: g.seq,
				Timestamp:      g.timestamp,
				SSRC:           uint32(g.ssrc),
				Marker:         isEnd, // RTP Marker bit set on final packet of frame
			},
			Payload: payload,
		}

		g.seq++
		packets = append(packets, pkt)
	}

	// Advance timestamp by samples per frame (90000 / fps)
	samplesPerFrame := uint32(VP8ClockRate / g.fps)
	g.timestamp += samplesPerFrame
	g.frameIdx++

	return packets
}

// Start launches the paced video sender loop.
func (g *VP8Generator) Start(ctx context.Context) {
	g.ctx, g.cancel = context.WithCancel(ctx)
	g.wg.Add(1)

	interval := time.Duration(1000/g.fps) * time.Millisecond
	go func() {
		defer g.wg.Done()
		ticker := time.NewTicker(interval)
		defer ticker.Stop()

		for {
			select {
			case <-g.ctx.Done():
				return
			case <-ticker.C:
				packets := g.GenerateFramePackets()
				for _, pkt := range packets {
					if g.track != nil {
						if err := g.track.WriteRTP(pkt); err == nil {
							g.packetsSent.Add(1)
							g.bytesSent.Add(uint64(pkt.MarshalSize()))
						}
					}
				}
			}
		}
	}()
}

// Stop terminates the media loop.
func (g *VP8Generator) Stop() {
	if g.cancel != nil {
		g.cancel()
	}
	g.wg.Wait()
}

// Stats returns total packets and bytes sent.
func (g *VP8Generator) Stats() (packets uint64, bytes uint64) {
	return g.packetsSent.Load(), g.bytesSent.Load()
}
