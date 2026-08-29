package metrics

import (
	"sort"
	"sync"
	"time"
)

// PeerMetrics stores performance and packet stats for an individual peer.
type PeerMetrics struct {
	PeerID    uint32 `json:"peer_id"`
	UserID    int64  `json:"user_id"`
	RoomID    uint64 `json:"room_id"`
	Role      string `json:"role"`
	Connected bool   `json:"connected"`

	JoinStart time.Time     `json:"join_start"`
	JoinTime  time.Duration `json:"join_duration_ms"`

	AudioPacketsSent uint64 `json:"audio_packets_sent"`
	VideoPacketsSent uint64 `json:"video_packets_sent"`
	AudioBytesSent   uint64 `json:"audio_bytes_sent"`
	VideoBytesSent   uint64 `json:"video_bytes_sent"`

	AudioPacketsReceived uint64 `json:"audio_packets_received"`
	VideoPacketsReceived uint64 `json:"video_packets_received"`
	AudioBytesReceived   uint64 `json:"audio_bytes_received"`
	VideoBytesReceived   uint64 `json:"video_bytes_received"`

	PacketsLost uint64  `json:"packets_lost"`
	LossRatePct float64 `json:"loss_rate_pct"`

	ErrorCount int    `json:"error_count"`
	LastError  string `json:"last_error,omitempty"`
}

// LatencySummary contains percentiles for join latency.
type LatencySummary struct {
	MinMs  float64 `json:"min_ms"`
	MeanMs float64 `json:"mean_ms"`
	P50Ms  float64 `json:"p50_ms"`
	P90Ms  float64 `json:"p90_ms"`
	P95Ms  float64 `json:"p95_ms"`
	P99Ms  float64 `json:"p99_ms"`
	MaxMs  float64 `json:"max_ms"`
}

// AggregatedSummary contains overall aggregated metrics across all rooms and peers.
type AggregatedSummary struct {
	TotalRooms     int     `json:"total_rooms"`
	TotalPeers     int     `json:"total_peers"`
	SpeakerCount   int     `json:"speaker_count"`
	AudienceCount  int     `json:"audience_count"`
	ConnectedPeers int     `json:"connected_peers"`
	FailedPeers    int     `json:"failed_peers"`
	SuccessRatePct float64 `json:"success_rate_pct"`

	DurationMs int64 `json:"duration_ms"`

	JoinLatency LatencySummary `json:"join_latency"`

	TotalAudioPacketsSent uint64 `json:"total_audio_packets_sent"`
	TotalVideoPacketsSent uint64 `json:"total_video_packets_sent"`
	TotalBytesSent        uint64 `json:"total_bytes_sent"`

	TotalAudioPacketsRecv uint64 `json:"total_audio_packets_recv"`
	TotalVideoPacketsRecv uint64 `json:"total_video_packets_recv"`
	TotalBytesRecv        uint64 `json:"total_bytes_recv"`

	TotalPacketsLost uint64  `json:"total_packets_lost"`
	AvgLossRatePct   float64 `json:"avg_loss_rate_pct"`

	TotalTxBitrateKbps float64 `json:"total_tx_bitrate_kbps"`
	TotalRxBitrateKbps float64 `json:"total_rx_bitrate_kbps"`
}

// Collector is a thread-safe registry of all peer metrics during a load test run.
type Collector struct {
	mu        sync.RWMutex
	startTime time.Time
	endTime   time.Time
	peers     map[string]*PeerMetrics
}

// NewCollector initializes a new Collector.
func NewCollector() *Collector {
	return &Collector{
		startTime: time.Now(),
		peers:     make(map[string]*PeerMetrics),
	}
}

// RegisterPeer creates or returns an entry for the specified peer.
func (c *Collector) RegisterPeer(key string, userID int64, roomID uint64, role string) *PeerMetrics {
	c.mu.Lock()
	defer c.mu.Unlock()

	pm := &PeerMetrics{
		UserID:    userID,
		RoomID:    roomID,
		Role:      role,
		JoinStart: time.Now(),
	}
	c.peers[key] = pm
	return pm
}

// RecordJoinSuccess records the time taken to successfully connect.
func (c *Collector) RecordJoinSuccess(key string) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if pm, ok := c.peers[key]; ok {
		pm.Connected = true
		pm.JoinTime = time.Since(pm.JoinStart)
	}
}

// RecordError increments the error count for a peer.
func (c *Collector) RecordError(key string, err error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if pm, ok := c.peers[key]; ok && err != nil {
		pm.ErrorCount++
		pm.LastError = err.Error()
	}
}

// UpdateMediaStats updates the transmitted/received counts for a peer.
func (c *Collector) UpdateMediaStats(key string, audioSent, videoSent, audioBytesSent, videoBytesSent, audioRecv, videoRecv, audioBytesRecv, videoBytesRecv, lost uint64) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if pm, ok := c.peers[key]; ok {
		pm.AudioPacketsSent = audioSent
		pm.VideoPacketsSent = videoSent
		pm.AudioBytesSent = audioBytesSent
		pm.VideoBytesSent = videoBytesSent

		pm.AudioPacketsReceived = audioRecv
		pm.VideoPacketsReceived = videoRecv
		pm.AudioBytesReceived = audioBytesRecv
		pm.VideoBytesReceived = videoBytesRecv
		pm.PacketsLost = lost

		totalRecvExpected := audioRecv + videoRecv + lost
		if totalRecvExpected > 0 {
			pm.LossRatePct = float64(lost) / float64(totalRecvExpected) * 100.0
		}
	}
}

// Stop marks the end of the collection period.
func (c *Collector) Stop() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.endTime = time.Now()
}

// Summary calculates and returns an aggregated summary of the run.
func (c *Collector) Summary() AggregatedSummary {
	c.mu.RLock()
	defer c.mu.RUnlock()

	endTime := c.endTime
	if endTime.IsZero() {
		endTime = time.Now()
	}
	duration := endTime.Sub(c.startTime)
	durationSec := duration.Seconds()
	if durationSec <= 0 {
		durationSec = 1
	}

	summary := AggregatedSummary{
		TotalPeers: len(c.peers),
		DurationMs: duration.Milliseconds(),
	}

	roomsMap := make(map[uint64]struct{})
	var joinLatencies []float64

	for _, pm := range c.peers {
		roomsMap[pm.RoomID] = struct{}{}
		if pm.Role == "speaker" {
			summary.SpeakerCount++
		} else {
			summary.AudienceCount++
		}

		if pm.Connected {
			summary.ConnectedPeers++
			latMs := float64(pm.JoinTime.Microseconds()) / 1000.0
			joinLatencies = append(joinLatencies, latMs)
		} else {
			summary.FailedPeers++
		}

		summary.TotalAudioPacketsSent += pm.AudioPacketsSent
		summary.TotalVideoPacketsSent += pm.VideoPacketsSent
		summary.TotalBytesSent += pm.AudioBytesSent + pm.VideoBytesSent

		summary.TotalAudioPacketsRecv += pm.AudioPacketsReceived
		summary.TotalVideoPacketsRecv += pm.VideoPacketsReceived
		summary.TotalBytesRecv += pm.AudioBytesReceived + pm.VideoBytesReceived
		summary.TotalPacketsLost += pm.PacketsLost
	}

	summary.TotalRooms = len(roomsMap)
	if summary.TotalPeers > 0 {
		summary.SuccessRatePct = float64(summary.ConnectedPeers) / float64(summary.TotalPeers) * 100.0
	}

	totalRecvExpected := summary.TotalAudioPacketsRecv + summary.TotalVideoPacketsRecv + summary.TotalPacketsLost
	if totalRecvExpected > 0 {
		summary.AvgLossRatePct = float64(summary.TotalPacketsLost) / float64(totalRecvExpected) * 100.0
	}

	summary.TotalTxBitrateKbps = (float64(summary.TotalBytesSent) * 8) / (durationSec * 1000)
	summary.TotalRxBitrateKbps = (float64(summary.TotalBytesRecv) * 8) / (durationSec * 1000)

	summary.JoinLatency = computeLatencySummary(joinLatencies)
	return summary
}

func computeLatencySummary(latencies []float64) LatencySummary {
	if len(latencies) == 0 {
		return LatencySummary{}
	}

	sort.Float64s(latencies)
	var sum float64
	for _, v := range latencies {
		sum += v
	}

	n := len(latencies)
	return LatencySummary{
		MinMs:  latencies[0],
		MaxMs:  latencies[n-1],
		MeanMs: sum / float64(n),
		P50Ms:  percentile(latencies, 0.50),
		P90Ms:  percentile(latencies, 0.90),
		P95Ms:  percentile(latencies, 0.95),
		P99Ms:  percentile(latencies, 0.99),
	}
}

func percentile(sorted []float64, pct float64) float64 {
	if len(sorted) == 0 {
		return 0
	}
	idx := int(float64(len(sorted)-1) * pct)
	return sorted[idx]
}
