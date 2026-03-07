package protocol

import (
	"bytes"
	"encoding/base64"
	"testing"
)

func TestFrameRoundTripBigEndian(t *testing.T) {
	buf := bytes.NewBuffer(nil)
	payload := []byte(`{"intent":"heartbeat","clientSequence":1,"heartbeatCounter":2}`)
	if err := WriteFrame(buf, payload); err != nil {
		t.Fatalf("WriteFrame failed: %v", err)
	}

	raw := buf.Bytes()
	if len(raw) < 4 {
		t.Fatalf("expected frame header")
	}
	wantLen := byte(len(payload))
	if raw[0] != 0 || raw[1] != 0 || raw[2] != 0 || raw[3] != wantLen {
		t.Fatalf("expected big-endian 4-byte length prefix, got %v", raw[:4])
	}

	decoded, err := ReadFrame(bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("ReadFrame failed: %v", err)
	}
	if !bytes.Equal(decoded, payload) {
		t.Fatalf("payload mismatch")
	}
}

func TestParseAndValidateClientIntent(t *testing.T) {
	valid := []byte(`{"intent":"hello","protocolVersion":1,"clientSequence":0}`)
	if _, violation := ParseAndValidateClientIntent(valid); violation != nil {
		t.Fatalf("unexpected violation: %v", violation)
	}

	invalid := []byte(`{"kind":"hello"}`)
	if _, violation := ParseAndValidateClientIntent(invalid); violation == nil {
		t.Fatalf("expected violation for missing discriminator")
	}
}

func TestPublishLinkEventPayloadBounds(t *testing.T) {
	oversized := bytes.Repeat([]byte{0xAB}, NetplayMaxLinkPayloadBytes+1)
	encoded := base64.StdEncoding.EncodeToString(oversized)
	payload := []byte(`{"intent":"publishLinkEvent","clientSequence":3,"event":{"sequence":1,"senderPlayerId":1,"tickMarker":5,"payload":"` + encoded + `"}}`)
	if _, violation := ParseAndValidateClientIntent(payload); violation == nil || violation.Code != 413 {
		t.Fatalf("expected 413 violation for oversized link payload, got %#v", violation)
	}
}
