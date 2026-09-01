package peer

import (
	"testing"

	"mezon-sfu/tools/loadtest/pkg/metrics"
)

func TestSeqTracker_LossCalculation(t *testing.T) {
	var st seqTracker

	// First packet initializes state, no loss
	if got := st.track(10); got != 0 {
		t.Errorf("expected 0 lost for first packet, got %d", got)
	}

	// Consecutive packet, no loss
	if got := st.track(11); got != 0 {
		t.Errorf("expected 0 lost for consecutive packets, got %d", got)
	}

	// Gap of 3 missing packets (11 -> 15 => lost 12, 13, 14 = 3 packets)
	if got := st.track(15); got != 3 {
		t.Errorf("expected 3 packets lost, got %d", got)
	}

	// Wraparound gap: 65534 -> 2 => diff = (2 - 65534) & 0xFFFF = 4, lost = 3
	if got := st.track(65534); got != 0 {
		t.Errorf("expected 0 lost for 15 -> 65534, got %d", got)
	}
	if got := st.track(2); got != 3 {
		t.Errorf("expected 3 packets lost after wraparound, got %d", got)
	}
}

func TestSeqTracker_IsolatedPerSSRC(t *testing.T) {
	// Distinct SSRCs must NOT interleave: no phantom loss across streams.
	var audioA, audioB seqTracker
	audioA.track(100)
	audioB.track(5000)

	for i := 0; i < 100; i++ {
		if got := audioA.track(uint32(101 + i)); got != 0 {
			t.Errorf("ssrc A: expected 0 lost at iter %d, got %d", i, got)
		}
		if got := audioB.track(uint32(5001 + i)); got != 0 {
			t.Errorf("ssrc B: expected 0 lost at iter %d, got %d", i, got)
		}
	}
}

func TestPeer_PeerKeyGeneration(t *testing.T) {
	c := metrics.NewCollector()
	cfg := Config{
		UserID: 42,
		RoomID: 999,
		Role:   "speaker",
	}
	p := NewPeer(cfg, c)
	if p.peerKey != "room_999_user_42" {
		t.Errorf("unexpected peer key: %s", p.peerKey)
	}
}
