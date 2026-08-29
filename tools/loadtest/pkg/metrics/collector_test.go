package metrics

import (
	"errors"
	"testing"
)

func TestCollector_RegisterAndSummary(t *testing.T) {
	c := NewCollector()

	p1 := c.RegisterPeer("p1", 1, 100, "speaker")
	p2 := c.RegisterPeer("p2", 2, 100, "audience")
	p3 := c.RegisterPeer("p3", 3, 101, "audience")

	if p1.UserID != 1 || p2.UserID != 2 || p3.UserID != 3 {
		t.Errorf("peer registration mismatch")
	}

	c.RecordJoinSuccess("p1")
	c.RecordJoinSuccess("p2")
	c.RecordError("p3", errors.New("timeout"))

	c.UpdateMediaStats("p1", 100, 200, 1000, 5000, 0, 0, 0, 0, 0)
	c.UpdateMediaStats("p2", 0, 0, 0, 0, 95, 190, 950, 4800, 5)

	c.Stop()
	summary := c.Summary()

	if summary.TotalRooms != 2 {
		t.Errorf("expected 2 rooms, got %d", summary.TotalRooms)
	}
	if summary.TotalPeers != 3 {
		t.Errorf("expected 3 peers, got %d", summary.TotalPeers)
	}
	if summary.ConnectedPeers != 2 || summary.FailedPeers != 1 {
		t.Errorf("expected 2 connected, 1 failed, got %d/%d", summary.ConnectedPeers, summary.FailedPeers)
	}
	if summary.SpeakerCount != 1 || summary.AudienceCount != 2 {
		t.Errorf("expected 1 speaker, 2 audience, got %d/%d", summary.SpeakerCount, summary.AudienceCount)
	}
	if summary.TotalAudioPacketsSent != 100 || summary.TotalVideoPacketsSent != 200 {
		t.Errorf("sent packets mismatch: audio %d, video %d", summary.TotalAudioPacketsSent, summary.TotalVideoPacketsSent)
	}
	if summary.TotalAudioPacketsRecv != 95 || summary.TotalVideoPacketsRecv != 190 {
		t.Errorf("recv packets mismatch: audio %d, video %d", summary.TotalAudioPacketsRecv, summary.TotalVideoPacketsRecv)
	}
	if summary.TotalPacketsLost != 5 {
		t.Errorf("expected 5 packets lost, got %d", summary.TotalPacketsLost)
	}
	if summary.SuccessRatePct < 66.0 || summary.SuccessRatePct > 67.0 {
		t.Errorf("unexpected success rate: %f", summary.SuccessRatePct)
	}
}

func TestComputeLatencySummary(t *testing.T) {
	latencies := []float64{10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0}
	summary := computeLatencySummary(latencies)

	if summary.MinMs != 10.0 || summary.MaxMs != 100.0 {
		t.Errorf("min/max mismatch: %f / %f", summary.MinMs, summary.MaxMs)
	}
	if summary.MeanMs != 55.0 {
		t.Errorf("mean mismatch: %f", summary.MeanMs)
	}
	if summary.P50Ms != 50.0 {
		t.Errorf("p50 mismatch: %f", summary.P50Ms)
	}
	if summary.P90Ms != 90.0 {
		t.Errorf("p90 mismatch: %f", summary.P90Ms)
	}
}
