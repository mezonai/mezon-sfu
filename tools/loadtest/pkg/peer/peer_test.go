package peer

import (
	"testing"

	"mezon-sfu/tools/loadtest/pkg/metrics"
)

func TestPeer_SequenceGapLossCalculation(t *testing.T) {
	c := metrics.NewCollector()
	cfg := Config{
		UserID: 1,
		RoomID: 100,
		Role:   "audience",
	}
	p := NewPeer(cfg, c)

	// In sequence
	p.checkSeqGap(10, 11)
	if p.packetsLost.Load() != 0 {
		t.Errorf("expected 0 packets lost for consecutive packets")
	}

	// Gap of 3 missing packets (11 -> 15 => lost 12, 13, 14 = 3 packets)
	p.checkSeqGap(11, 15)
	if p.packetsLost.Load() != 3 {
		t.Errorf("expected 3 packets lost, got %d", p.packetsLost.Load())
	}

	// Wraparound gap: 65534 -> 2 => diff = (2 - 65534) & 0xFFFF = 4, lost = 3
	p.checkSeqGap(65534, 2)
	if p.packetsLost.Load() != 6 {
		t.Errorf("expected 6 total packets lost after wraparound, got %d", p.packetsLost.Load())
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
