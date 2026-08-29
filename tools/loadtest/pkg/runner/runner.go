package runner

import (
	"context"
	"sync"
	"time"

	"mezon-sfu/tools/loadtest/pkg/metrics"
	"mezon-sfu/tools/loadtest/pkg/peer"
	"mezon-sfu/tools/loadtest/pkg/report"
)

// Config configures the runner execution.
type Config struct {
	WSURL           string
	JWTSecret       string
	Rooms           int
	PeersPerRoom    int
	SpeakersPerRoom int
	RoomStartID     uint64
	UserStartID     int64
	Duration        time.Duration
	RampDuration    time.Duration
	FPS             int
	BitrateBps      int
	Thresholds      report.Thresholds
}

// Runner orchestrates the multi-room, multi-peer load test.
type Runner struct {
	cfg       Config
	collector *metrics.Collector
	peers     []*peer.Peer
	mu        sync.Mutex
}

// NewRunner creates a new load test runner.
func NewRunner(cfg Config) *Runner {
	if cfg.Rooms <= 0 {
		cfg.Rooms = 30
	}
	if cfg.PeersPerRoom <= 0 {
		cfg.PeersPerRoom = 10
	}
	if cfg.SpeakersPerRoom < 0 {
		cfg.SpeakersPerRoom = 2
	}
	if cfg.RoomStartID == 0 {
		cfg.RoomStartID = 1000
	}
	if cfg.UserStartID == 0 {
		cfg.UserStartID = 10000
	}
	if cfg.Duration <= 0 {
		cfg.Duration = 30 * time.Second
	}
	if cfg.RampDuration < 0 {
		cfg.RampDuration = 5 * time.Second
	}
	if cfg.FPS <= 0 {
		cfg.FPS = 30
	}
	if cfg.BitrateBps <= 0 {
		cfg.BitrateBps = 240000
	}

	return &Runner{
		cfg:       cfg,
		collector: metrics.NewCollector(),
	}
}

// Run executes the load test across all configured rooms and peers.
func (r *Runner) Run(ctx context.Context) (report.TestReport, error) {
	totalPeers := r.cfg.Rooms * r.cfg.PeersPerRoom
	var rampInterval time.Duration
	if r.cfg.RampDuration > 0 && totalPeers > 1 {
		rampInterval = r.cfg.RampDuration / time.Duration(totalPeers)
	}

	// Prepare peer instances
	var peerConfigs []peer.Config
	for roomIdx := 0; roomIdx < r.cfg.Rooms; roomIdx++ {
		roomID := r.cfg.RoomStartID + uint64(roomIdx)
		for pIdx := 0; pIdx < r.cfg.PeersPerRoom; pIdx++ {
			userID := r.cfg.UserStartID + int64(roomIdx*1000) + int64(pIdx)
			role := "audience"
			if pIdx < r.cfg.SpeakersPerRoom {
				role = "speaker"
			}

			peerConfigs = append(peerConfigs, peer.Config{
				UserID:     userID,
				RoomID:     roomID,
				Role:       role,
				JWTSecret:  r.cfg.JWTSecret,
				WSURL:      r.cfg.WSURL,
				FPS:        r.cfg.FPS,
				BitrateBps: r.cfg.BitrateBps,
			})
		}
	}

	var wg sync.WaitGroup
	for i, pCfg := range peerConfigs {
		select {
		case <-ctx.Done():
			r.teardown()
			return report.TestReport{}, ctx.Err()
		default:
		}

		p := peer.NewPeer(pCfg, r.collector)
		r.mu.Lock()
		r.peers = append(r.peers, p)
		r.mu.Unlock()

		wg.Add(1)
		go func(p *peer.Peer) {
			defer wg.Done()
			if err := p.Start(ctx); err != nil {
				// Error recorded in collector by peer
			}
		}(p)

		if rampInterval > 0 && i < len(peerConfigs)-1 {
			select {
			case <-ctx.Done():
				r.teardown()
				return report.TestReport{}, ctx.Err()
			case <-time.After(rampInterval):
			}
		}
	}

	// Wait for all peers to launch
	wg.Wait()

	// Run steady-state duration
	select {
	case <-ctx.Done():
	case <-time.After(r.cfg.Duration):
	}

	// Teardown all peers
	r.teardown()

	r.collector.Stop()
	summary := r.collector.Summary()

	repCfg := report.TestConfig{
		WSURL:        r.cfg.WSURL,
		Rooms:        r.cfg.Rooms,
		PeersPerRoom: r.cfg.PeersPerRoom,
		Speakers:     r.cfg.SpeakersPerRoom,
		Duration:     r.cfg.Duration,
		RampDuration: r.cfg.RampDuration,
		FPS:          r.cfg.FPS,
		BitrateBps:   r.cfg.BitrateBps,
	}

	testReport := report.GenerateReport(repCfg, summary, r.cfg.Thresholds)
	return testReport, nil
}

func (r *Runner) teardown() {
	r.mu.Lock()
	defer r.mu.Unlock()

	var wg sync.WaitGroup
	for _, p := range r.peers {
		wg.Add(1)
		go func(p *peer.Peer) {
			defer wg.Done()
			_ = p.Close()
		}(p)
	}
	wg.Wait()
}
