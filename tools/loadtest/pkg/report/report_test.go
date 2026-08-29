package report

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"mezon-sfu/tools/loadtest/pkg/metrics"
)

func TestEvaluateThresholds_PassAndFail(t *testing.T) {
	summary := metrics.AggregatedSummary{
		TotalPeers:     10,
		ConnectedPeers: 10,
		SuccessRatePct: 100.0,
		AvgLossRatePct: 1.2,
		JoinLatency: metrics.LatencySummary{
			P95Ms: 150.0,
		},
		TotalAudioPacketsRecv: 500,
		TotalVideoPacketsRecv: 1000,
		AudienceCount:         8,
	}

	// Passing thresholds
	passTh := Thresholds{
		MinSuccessRatePct: 95.0,
		MaxP95JoinTimeMs:  500.0,
		MaxPacketLossPct:  5.0,
		MinRxPackets:      1000,
	}

	results, passed := EvaluateThresholds(summary, passTh)
	if !passed {
		t.Errorf("expected thresholds to pass, got failed")
	}
	if len(results) != 4 {
		t.Errorf("expected 4 threshold results, got %d", len(results))
	}

	// Failing thresholds (due to strict latency & loss)
	failTh := Thresholds{
		MinSuccessRatePct: 95.0,
		MaxP95JoinTimeMs:  100.0, // actual is 150.0
		MaxPacketLossPct:  1.0,   // actual is 1.2
	}

	results, passed = EvaluateThresholds(summary, failTh)
	if passed {
		t.Errorf("expected thresholds to fail, got passed")
	}
}

func TestGenerateReport_ReadableAndJSON(t *testing.T) {
	cfg := TestConfig{
		WSURL:        "ws://127.0.0.1:8000/ws",
		Rooms:        2,
		PeersPerRoom: 5,
		Speakers:     1,
		Duration:     10 * time.Second,
		RampDuration: 2 * time.Second,
		FPS:          30,
		BitrateBps:   500000,
	}

	summary := metrics.AggregatedSummary{
		TotalRooms:     2,
		TotalPeers:     10,
		SpeakerCount:   2,
		AudienceCount:  8,
		ConnectedPeers: 10,
		SuccessRatePct: 100.0,
		JoinLatency: metrics.LatencySummary{
			MinMs:  20.0,
			MeanMs: 50.0,
			P50Ms:  45.0,
			P90Ms:  80.0,
			P95Ms:  90.0,
			P99Ms:  98.0,
			MaxMs:  100.0,
		},
		TotalAudioPacketsSent: 1000,
		TotalVideoPacketsSent: 3000,
		TotalBytesSent:        1000000,
		TotalAudioPacketsRecv: 8000,
		TotalVideoPacketsRecv: 24000,
		TotalBytesRecv:        8000000,
		AvgLossRatePct:        0.5,
	}

	th := Thresholds{
		MinSuccessRatePct: 95.0,
		MaxP95JoinTimeMs:  1000.0,
		MaxPacketLossPct:  5.0,
	}

	rep := GenerateReport(cfg, summary, th)
	if !rep.Passed {
		t.Errorf("expected report to pass")
	}

	readable := rep.FormatReadable()
	if !strings.Contains(readable, "OVERALL VERDICT: PASS") {
		t.Errorf("readable report missing PASS verdict: %s", readable)
	}
	if !strings.Contains(readable, "2 Rooms x 5 Peers") {
		t.Errorf("readable report missing topology details: %s", readable)
	}

	var buf bytes.Buffer
	if err := rep.WriteJSON(&buf); err != nil {
		t.Fatalf("WriteJSON failed: %v", err)
	}

	var parsed TestReport
	if err := json.Unmarshal(buf.Bytes(), &parsed); err != nil {
		t.Fatalf("failed to unmarshal JSON report: %v", err)
	}
	if parsed.Config.Rooms != 2 || parsed.Summary.TotalPeers != 10 || !parsed.Passed {
		t.Errorf("parsed JSON report mismatch: %+v", parsed)
	}
}
