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

func TestCreateRoomMaxPlayersRange(t *testing.T) {
	tooSmall := []byte(`{"intent":"createRoom","clientSequence":1,"roomName":"r","maxPlayers":1}`)
	if _, violation := ParseAndValidateClientIntent(tooSmall); violation == nil || violation.Code != 400 {
		t.Fatalf("expected 400 for maxPlayers<2, got %#v", violation)
	}
	tooLarge := []byte(`{"intent":"createRoom","clientSequence":1,"roomName":"r","maxPlayers":5}`)
	if _, violation := ParseAndValidateClientIntent(tooLarge); violation == nil || violation.Code != 400 {
		t.Fatalf("expected 400 for maxPlayers>4, got %#v", violation)
	}
}

func TestProtocolVersionMismatchDeterministicAcrossIntents(t *testing.T) {
	tests := []string{
		`{"intent":"createRoom","clientSequence":1,"protocolVersion":2,"roomName":"r","maxPlayers":2}`,
		`{"intent":"joinRoom","clientSequence":1,"protocolVersion":2,"roomId":"r"}`,
		`{"intent":"leaveRoom","clientSequence":1,"protocolVersion":2,"roomId":"r"}`,
		`{"intent":"heartbeat","clientSequence":1,"protocolVersion":2,"heartbeatCounter":1}`,
		`{"intent":"publishLinkEvent","clientSequence":1,"protocolVersion":2,"event":{"sequence":1,"senderPlayerId":1,"tickMarker":1,"payload":"AQ=="}}`,
	}
	for _, payload := range tests {
		if _, violation := ParseAndValidateClientIntent([]byte(payload)); violation == nil || violation.Code != 426 {
			t.Fatalf("expected 426 protocol mismatch for %s, got %#v", payload, violation)
		}
	}
}

func TestClientSequenceValidation(t *testing.T) {
	tests := []struct {
		name    string
		payload string
	}{
		{
			name:    "non-numeric",
			payload: `{"intent":"heartbeat","protocolVersion":1,"clientSequence":"abc","heartbeatCounter":1}`,
		},
		{
			name:    "negative",
			payload: `{"intent":"heartbeat","protocolVersion":1,"clientSequence":-1,"heartbeatCounter":1}`,
		},
		{
			name:    "fractional",
			payload: `{"intent":"heartbeat","protocolVersion":1,"clientSequence":1.5,"heartbeatCounter":1}`,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if _, violation := ParseAndValidateClientIntent([]byte(tc.payload)); violation == nil || violation.Code != 400 {
				t.Fatalf("expected 400 violation for %s, got %#v", tc.name, violation)
			}
		})
	}
}
