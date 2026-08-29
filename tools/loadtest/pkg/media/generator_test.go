package media

import (
	"context"
	"testing"
	"time"
)

func TestOpusGenerator_PacketStructure(t *testing.T) {
	gen := NewOpusGenerator(nil, 111, 12345)
	pkt1 := gen.GeneratePacket()
	pkt2 := gen.GeneratePacket()

	if pkt1.PayloadType != 111 {
		t.Errorf("expected payload type 111, got %d", pkt1.PayloadType)
	}
	if pkt1.SSRC != 12345 {
		t.Errorf("expected SSRC 12345, got %d", pkt1.SSRC)
	}
	if pkt2.SequenceNumber != pkt1.SequenceNumber+1 {
		t.Errorf("expected sequence number increment: %d -> %d", pkt1.SequenceNumber, pkt2.SequenceNumber)
	}
	if pkt2.Timestamp != pkt1.Timestamp+OpusSamplesPerFrame {
		t.Errorf("expected timestamp delta %d, got %d", OpusSamplesPerFrame, pkt2.Timestamp-pkt1.Timestamp)
	}
	if len(pkt1.Payload) == 0 {
		t.Errorf("expected non-empty opus payload")
	}
}

func TestVP8Generator_KeyframeAndPacketStructure(t *testing.T) {
	gen := NewVP8Generator(nil, 96, 54321, 30, 500000)

	// First frame must be a keyframe
	packets := gen.GenerateFramePackets()
	if len(packets) == 0 {
		t.Fatalf("expected at least 1 packet for video frame")
	}

	firstPkt := packets[0]
	lastPkt := packets[len(packets)-1]

	if firstPkt.PayloadType != 96 {
		t.Errorf("expected PT 96, got %d", firstPkt.PayloadType)
	}
	if firstPkt.SSRC != 54321 {
		t.Errorf("expected SSRC 54321, got %d", firstPkt.SSRC)
	}
	// First packet must have S bit set (0x10)
	if len(firstPkt.Payload) < 2 || firstPkt.Payload[0]&0x10 == 0 {
		t.Errorf("expected S bit set in first packet descriptor: %x", firstPkt.Payload[0])
	}
	// Last packet must have RTP Marker set
	if !lastPkt.Marker {
		t.Errorf("expected Marker bit set on last packet of frame")
	}

	// Verify keyframe start code in payload of first packet
	// Payload[0] is descriptor, Payload[1..3] is uncompressed header, Payload[4..6] is 0x9d 0x01 0x2a
	if len(firstPkt.Payload) < 7 {
		t.Fatalf("payload too short for keyframe")
	}
	if firstPkt.Payload[4] != 0x9d || firstPkt.Payload[5] != 0x01 || firstPkt.Payload[6] != 0x2a {
		t.Errorf("expected VP8 keyframe sync code 0x9d012a, got %x %x %x",
			firstPkt.Payload[4], firstPkt.Payload[5], firstPkt.Payload[6])
	}

	// Second frame (delta frame)
	deltaPackets := gen.GenerateFramePackets()
	if len(deltaPackets) == 0 {
		t.Fatalf("expected at least 1 packet for delta frame")
	}
	deltaFirst := deltaPackets[0]
	// Interframe bit 0 in uncompressed header is 1
	if deltaFirst.Payload[1]&0x01 != 1 {
		t.Errorf("expected interframe bit set to 1 in delta frame header: %x", deltaFirst.Payload[1])
	}
}

func TestMediaGenerator_StartStop(t *testing.T) {
	opusGen := NewOpusGenerator(nil, 111, 100)
	vp8Gen := NewVP8Generator(nil, 96, 200, 30, 200000)

	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()

	opusGen.Start(ctx)
	vp8Gen.Start(ctx)

	time.Sleep(60 * time.Millisecond)

	opusGen.Stop()
	vp8Gen.Stop()
}
