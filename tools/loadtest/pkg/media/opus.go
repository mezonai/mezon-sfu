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
	OpusClockRate          = 48000
	OpusFrameDuration      = 20 * time.Millisecond     // 50 pps
	OpusSamplesPerFrame    = OpusClockRate * 20 / 1000 // 960
	OpusDefaultPayloadType = 111
)

// OpusGenerator generates synthetic paced Opus RTP packets.
type OpusGenerator struct {
	track       *webrtc.TrackLocalStaticRTP
	payloadType uint8
	ssrc        uint32

	seq       uint16
	timestamp uint32

	packetsSent atomic.Uint64
	bytesSent   atomic.Uint64

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// NewOpusGenerator creates a new generator attached to a Pion local track.
func NewOpusGenerator(track *webrtc.TrackLocalStaticRTP, payloadType uint8, ssrc uint32) *OpusGenerator {
	if payloadType == 0 {
		payloadType = OpusDefaultPayloadType
	}
	if ssrc == 0 {
		ssrc = rand.Uint32()
	}
	return &OpusGenerator{
		track:       track,
		payloadType: payloadType,
		ssrc:        ssrc,
		seq:         uint16(rand.Intn(65535)),
		timestamp:   rand.Uint32(),
	}
}

// GeneratePacket builds a single synthetic Opus RTP packet with incrementing seq and timestamp.
func (g *OpusGenerator) GeneratePacket() *rtp.Packet {
	// Synthetic Opus comfort/speech payload (stereo silence/noise frame)
	payload := []byte{0xf8, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00}

	pkt := &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    g.payloadType,
			SequenceNumber: g.seq,
			Timestamp:      g.timestamp,
			SSRC:           uint32(g.ssrc),
			Marker:         false,
		},
		Payload: payload,
	}

	g.seq++
	g.timestamp += OpusSamplesPerFrame
	return pkt
}

// Start launches the paced packet generator loop in the background.
func (g *OpusGenerator) Start(ctx context.Context) {
	g.ctx, g.cancel = context.WithCancel(ctx)
	g.wg.Add(1)

	go func() {
		defer g.wg.Done()
		ticker := time.NewTicker(OpusFrameDuration)
		defer ticker.Stop()

		for {
			select {
			case <-g.ctx.Done():
				return
			case <-ticker.C:
				pkt := g.GeneratePacket()
				if g.track != nil {
					if err := g.track.WriteRTP(pkt); err == nil {
						g.packetsSent.Add(1)
						g.bytesSent.Add(uint64(pkt.MarshalSize()))
					}
				}
			}
		}
	}()
}

// Stop terminates the media loop.
func (g *OpusGenerator) Stop() {
	if g.cancel != nil {
		g.cancel()
	}
	g.wg.Wait()
}

// Stats returns the total packets and bytes sent.
func (g *OpusGenerator) Stats() (packets uint64, bytes uint64) {
	return g.packetsSent.Load(), g.bytesSent.Load()
}
