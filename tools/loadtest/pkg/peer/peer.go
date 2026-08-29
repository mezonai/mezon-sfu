package peer

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/rtcp"
	"github.com/pion/webrtc/v4"

	"mezon-sfu/tools/loadtest/pkg/auth"
	"mezon-sfu/tools/loadtest/pkg/media"
	"mezon-sfu/tools/loadtest/pkg/metrics"
	"mezon-sfu/tools/loadtest/pkg/signaling"
)

// Config holds configuration parameters for a peer session.
type Config struct {
	UserID     int64
	RoomID     uint64
	Role       string // "speaker" or "audience"
	JWTSecret  string
	WSURL      string
	FPS        int
	BitrateBps int
}

// Peer encapsulates a single WebRTC participant connected via WebSocket and Pion.
type Peer struct {
	cfg       Config
	peerKey   string
	collector *metrics.Collector

	wsClient *signaling.Client
	pc       *webrtc.PeerConnection

	audioTrack *webrtc.TrackLocalStaticRTP
	videoTrack *webrtc.TrackLocalStaticRTP
	audioGen   *media.OpusGenerator
	videoGen   *media.VP8Generator

	mu            sync.Mutex
	isNegotiating bool
	pendingOffer  *signaling.OfferMessage
	connectedOnce atomic.Bool
	closed        atomic.Bool
	ctx           context.Context
	cancel        context.CancelFunc
	wg            sync.WaitGroup

	// RX Stats tracking
	audioRxPackets atomic.Uint64
	videoRxPackets atomic.Uint64
	audioRxBytes   atomic.Uint64
	videoRxBytes   atomic.Uint64
	packetsLost    atomic.Uint64

	lastAudioSeq atomic.Uint32
	lastVideoSeq atomic.Uint32
	hasAudioSeq  atomic.Bool
	hasVideoSeq  atomic.Bool
}

// NewPeer constructs a new Peer instance.
func NewPeer(cfg Config, collector *metrics.Collector) *Peer {
	peerKey := fmt.Sprintf("room_%d_user_%d", cfg.RoomID, cfg.UserID)
	if collector != nil {
		collector.RegisterPeer(peerKey, cfg.UserID, cfg.RoomID, cfg.Role)
	}

	return &Peer{
		cfg:       cfg,
		peerKey:   peerKey,
		collector: collector,
	}
}

// Start initiates the WebSocket signaling and WebRTC connection.
func (p *Peer) Start(ctx context.Context) error {
	p.ctx, p.cancel = context.WithCancel(ctx)

	// 1. Create Pion PeerConnection
	mediaEngine := &webrtc.MediaEngine{}

	// Register Opus codec
	if err := mediaEngine.RegisterCodec(webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType:     webrtc.MimeTypeOpus,
			ClockRate:    media.OpusClockRate,
			Channels:     2,
			SDPFmtpLine:  "",
			RTCPFeedback: nil,
		},
		PayloadType: media.OpusDefaultPayloadType,
	}, webrtc.RTPCodecTypeAudio); err != nil {
		return fmt.Errorf("failed to register opus codec: %w", err)
	}

	// Register VP8 codec with NACK/PLI/TWCC feedback
	if err := mediaEngine.RegisterCodec(webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType:    webrtc.MimeTypeVP8,
			ClockRate:   media.VP8ClockRate,
			Channels:    0,
			SDPFmtpLine: "",
			RTCPFeedback: []webrtc.RTCPFeedback{
				{Type: "nack", Parameter: ""},
				{Type: "nack", Parameter: "pli"},
				{Type: "transport-cc", Parameter: ""},
			},
		},
		PayloadType: media.VP8DefaultPayloadType,
	}, webrtc.RTPCodecTypeVideo); err != nil {
		return fmt.Errorf("failed to register vp8 codec: %w", err)
	}

	api := webrtc.NewAPI(webrtc.WithMediaEngine(mediaEngine))
	pc, err := api.NewPeerConnection(webrtc.Configuration{
		SDPSemantics: webrtc.SDPSemanticsUnifiedPlan,
	})
	if err != nil {
		return fmt.Errorf("failed to create peer connection: %w", err)
	}
	p.pc = pc

	// 2. Set up Track Event Handlers
	pc.OnTrack(func(track *webrtc.TrackRemote, receiver *webrtc.RTPReceiver) {
		p.handleRemoteTrack(track)
	})

	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		if state == webrtc.PeerConnectionStateConnected {
			if p.connectedOnce.CompareAndSwap(false, true) {
				if p.collector != nil {
					p.collector.RecordJoinSuccess(p.peerKey)
				}
				if p.cfg.Role == "speaker" {
					if p.audioGen != nil {
						p.audioGen.Start(p.ctx)
					}
					if p.videoGen != nil {
						p.videoGen.Start(p.ctx)
					}
					// Signal camera is active
					_ = p.wsClient.SendCamera(true)
				}
			}
		}
	})

	// 3. If Speaker, prepare local media tracks and generators
	if p.cfg.Role == "speaker" {
		audioTrack, err := webrtc.NewTrackLocalStaticRTP(
			webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus},
			"audio",
			fmt.Sprintf("user-%d-stream", p.cfg.UserID),
		)
		if err != nil {
			return fmt.Errorf("failed to create local audio track: %w", err)
		}
		p.audioTrack = audioTrack
		if _, err := pc.AddTrack(audioTrack); err != nil {
			return fmt.Errorf("failed to add audio track to pc: %w", err)
		}
		p.audioGen = media.NewOpusGenerator(audioTrack, media.OpusDefaultPayloadType, 0)

		videoTrack, err := webrtc.NewTrackLocalStaticRTP(
			webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8},
			"video",
			fmt.Sprintf("user-%d-stream", p.cfg.UserID),
		)
		if err != nil {
			return fmt.Errorf("failed to create local video track: %w", err)
		}
		p.videoTrack = videoTrack
		videoSender, err := pc.AddTrack(videoTrack)
		if err != nil {
			return fmt.Errorf("failed to add video track to pc: %w", err)
		}
		p.videoGen = media.NewVP8Generator(videoTrack, media.VP8DefaultPayloadType, 0, p.cfg.FPS, p.cfg.BitrateBps)

		// Read RTCP feedback from video sender to react to PLI/FIR
		go p.readRTCP(videoSender)
	}

	// 4. Set up Signaling Client
	handlers := signaling.ClientHandlers{
		OnJoined: func(msg signaling.JoinedMessage) {
			// Room joined successfully
		},
		OnOffer: func(offer signaling.OfferMessage) {
			p.handleServerOffer(offer)
		},
		OnError: func(err signaling.ErrorMessage) {
			if p.collector != nil {
				p.collector.RecordError(p.peerKey, fmt.Errorf("signaling error: %s", err.Message))
			}
		},
		OnClose: func(err error) {
			if err != nil && p.collector != nil && !p.closed.Load() {
				p.collector.RecordError(p.peerKey, err)
			}
		},
	}

	p.wsClient = signaling.NewClient(p.cfg.WSURL, handlers)
	if err := p.wsClient.Connect(p.ctx); err != nil {
		if p.collector != nil {
			p.collector.RecordError(p.peerKey, err)
		}
		return fmt.Errorf("signaling connect failed: %w", err)
	}

	// 5. Generate JWT and Send Join
	token, err := auth.GenerateToken(p.cfg.JWTSecret, p.cfg.UserID, p.cfg.RoomID, 24*time.Hour)
	if err != nil {
		return fmt.Errorf("failed to generate jwt: %w", err)
	}

	if err := p.wsClient.Join(token, p.cfg.Role, "vp8"); err != nil {
		return fmt.Errorf("failed to send join: %w", err)
	}

	// Launch background stats reporter
	go p.statsReporter()

	return nil
}

func (p *Peer) readRTCP(sender *webrtc.RTPSender) {
	for {
		if p.closed.Load() {
			return
		}
		pkts, _, err := sender.ReadRTCP()
		if err != nil {
			return
		}
		for _, pkt := range pkts {
			switch pkt.(type) {
			case *rtcp.PictureLossIndication, *rtcp.FullIntraRequest:
				if p.videoGen != nil {
					p.videoGen.RequestKeyframe()
				}
			}
		}
	}
}

func (p *Peer) handleRemoteTrack(track *webrtc.TrackRemote) {
	p.wg.Add(1)
	go func() {
		defer p.wg.Done()
		isAudio := track.Kind() == webrtc.RTPCodecTypeAudio

		for {
			if p.closed.Load() {
				return
			}

			pkt, _, err := track.ReadRTP()
			if err != nil {
				if errors.Is(err, io.EOF) || p.closed.Load() {
					return
				}
				continue
			}

			size := uint64(pkt.MarshalSize())
			seq := uint32(pkt.SequenceNumber)

			if isAudio {
				p.audioRxPackets.Add(1)
				p.audioRxBytes.Add(size)

				if p.hasAudioSeq.CompareAndSwap(false, true) {
					p.lastAudioSeq.Store(seq)
				} else {
					prev := p.lastAudioSeq.Swap(seq)
					p.checkSeqGap(prev, seq)
				}
			} else {
				p.videoRxPackets.Add(1)
				p.videoRxBytes.Add(size)

				if p.hasVideoSeq.CompareAndSwap(false, true) {
					p.lastVideoSeq.Store(seq)
				} else {
					prev := p.lastVideoSeq.Swap(seq)
					p.checkSeqGap(prev, seq)
				}
			}
		}
	}()
}

func (p *Peer) checkSeqGap(prev, cur uint32) {
	// Standard RTP sequence number gap check (modulo 65536)
	diff := (cur - prev) & 0xFFFF
	if diff > 1 && diff < 3000 { // Gap detected, packets lost
		lost := uint64(diff - 1)
		p.packetsLost.Add(lost)
	}
}

func (p *Peer) handleServerOffer(offer signaling.OfferMessage) {
	p.mu.Lock()
	if p.isNegotiating {
		// Queue latest offer
		p.pendingOffer = &offer
		p.mu.Unlock()
		return
	}
	p.isNegotiating = true
	p.mu.Unlock()

	currentOffer := &offer
	for currentOffer != nil {
		if p.closed.Load() {
			return
		}

		err := p.processOffer(*currentOffer)
		if err != nil {
			if p.collector != nil {
				p.collector.RecordError(p.peerKey, fmt.Errorf("negotiation error: %w", err))
			}
		}

		p.mu.Lock()
		currentOffer = p.pendingOffer
		p.pendingOffer = nil
		if currentOffer == nil {
			p.isNegotiating = false
		}
		p.mu.Unlock()
	}
}

func (p *Peer) processOffer(offer signaling.OfferMessage) error {
	sdp := webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer,
		SDP:  offer.SDP,
	}

	if err := p.pc.SetRemoteDescription(sdp); err != nil {
		return fmt.Errorf("SetRemoteDescription failed: %w", err)
	}

	answer, err := p.pc.CreateAnswer(nil)
	if err != nil {
		return fmt.Errorf("CreateAnswer failed: %w", err)
	}

	if err := p.pc.SetLocalDescription(answer); err != nil {
		return fmt.Errorf("SetLocalDescription failed: %w", err)
	}

	if err := p.wsClient.SendAnswer(answer.SDP, offer.OfferGeneration); err != nil {
		return fmt.Errorf("SendAnswer failed: %w", err)
	}

	return nil
}

func (p *Peer) statsReporter() {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-p.ctx.Done():
			p.flushStats()
			return
		case <-ticker.C:
			p.flushStats()
		}
	}
}

func (p *Peer) flushStats() {
	if p.collector == nil {
		return
	}

	var aSent, vSent, aBSent, vBSent uint64
	if p.audioGen != nil {
		aSent, aBSent = p.audioGen.Stats()
	}
	if p.videoGen != nil {
		vSent, vBSent = p.videoGen.Stats()
	}

	p.collector.UpdateMediaStats(
		p.peerKey,
		aSent,
		vSent,
		aBSent,
		vBSent,
		p.audioRxPackets.Load(),
		p.videoRxPackets.Load(),
		p.audioRxBytes.Load(),
		p.videoRxBytes.Load(),
		p.packetsLost.Load(),
	)
}

// Close terminates peer connection, signaling, and media generators.
func (p *Peer) Close() error {
	if p.closed.CompareAndSwap(false, true) {
		if p.cancel != nil {
			p.cancel()
		}
		if p.audioGen != nil {
			p.audioGen.Stop()
		}
		if p.videoGen != nil {
			p.videoGen.Stop()
		}
		if p.wsClient != nil {
			_ = p.wsClient.Close()
		}
		if p.pc != nil {
			_ = p.pc.Close()
		}
		p.flushStats()
		p.wg.Wait()
	}
	return nil
}
