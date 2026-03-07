package protocol

import (
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
)

const (
	SupportedProtocolVersion   = 1
	NetplayMaxFramePayloadSize = 4096
	NetplayMaxLinkPayloadBytes = 1024
)

type ProtocolViolation struct {
	Code    int
	Message string
	Reason  string
}

func (v *ProtocolViolation) Error() string {
	return v.Message
}

type LinkEvent struct {
	Sequence       int64  `json:"sequence"`
	SenderPlayerID int    `json:"senderPlayerId"`
	TickMarker     int64  `json:"tickMarker"`
	Payload        string `json:"payload"`
}

type ClientIntent struct {
	Intent          string          `json:"intent"`
	ClientSequence  int64           `json:"clientSequence"`
	ProtocolVersion int             `json:"protocolVersion,omitempty"`
	AuthToken       string          `json:"authToken,omitempty"`
	RoomName        string          `json:"roomName,omitempty"`
	MaxPlayers      int             `json:"maxPlayers,omitempty"`
	RoomID          string          `json:"roomId,omitempty"`
	Heartbeat       int64           `json:"heartbeatCounter,omitempty"`
	Event           json.RawMessage `json:"event,omitempty"`
}

func WriteFrame(w io.Writer, payload []byte) error {
	if len(payload) == 0 || len(payload) > NetplayMaxFramePayloadSize {
		return &ProtocolViolation{Code: 413, Message: "payload size violates NETPLAY_MAX_FRAME_PAYLOAD_BYTES"}
	}
	var header [4]byte
	binary.BigEndian.PutUint32(header[:], uint32(len(payload)))
	if _, err := w.Write(header[:]); err != nil {
		return fmt.Errorf("write frame header: %w", err)
	}
	if _, err := w.Write(payload); err != nil {
		return fmt.Errorf("write frame payload: %w", err)
	}
	return nil
}

func ReadFrame(r io.Reader) ([]byte, error) {
	var header [4]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint32(header[:])
	if length == 0 || length > NetplayMaxFramePayloadSize {
		return nil, &ProtocolViolation{Code: 400, Message: fmt.Sprintf("invalid frame length: %d", length), Reason: "protocolError"}
	}
	payload := make([]byte, int(length))
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

func ParseAndValidateClientIntent(payload []byte) (*ClientIntent, *ProtocolViolation) {
	if len(payload) == 0 {
		return nil, &ProtocolViolation{Code: 400, Message: "empty payload", Reason: "protocolError"}
	}
	if len(payload) > NetplayMaxFramePayloadSize {
		return nil, &ProtocolViolation{Code: 413, Message: "payload too large", Reason: "protocolError"}
	}

	var root map[string]json.RawMessage
	if err := json.Unmarshal(payload, &root); err != nil {
		return nil, &ProtocolViolation{Code: 400, Message: fmt.Sprintf("invalid JSON payload: %v", err), Reason: "protocolError"}
	}

	intentRaw, ok := root["intent"]
	if !ok {
		return nil, &ProtocolViolation{Code: 400, Message: "missing top-level discriminator: intent", Reason: "protocolError"}
	}
	var intent string
	if err := json.Unmarshal(intentRaw, &intent); err != nil || intent == "" {
		return nil, &ProtocolViolation{Code: 400, Message: "intent must be a non-empty string", Reason: "protocolError"}
	}

	var decoded ClientIntent
	if err := json.Unmarshal(payload, &decoded); err != nil {
		return nil, &ProtocolViolation{Code: 400, Message: fmt.Sprintf("invalid intent payload: %v", err), Reason: "protocolError"}
	}

	requireProtocol := func() *ProtocolViolation {
		if _, ok := root["protocolVersion"]; !ok {
			return &ProtocolViolation{Code: 400, Message: "missing required field: protocolVersion", Reason: "protocolError"}
		}
		if decoded.ProtocolVersion != SupportedProtocolVersion {
			return &ProtocolViolation{Code: 426, Message: fmt.Sprintf("unsupported protocolVersion: %d. Expected: %d", decoded.ProtocolVersion, SupportedProtocolVersion), Reason: "protocolError"}
		}
		return nil
	}

	requireString := func(name, value string) *ProtocolViolation {
		if _, ok := root[name]; !ok || value == "" {
			return &ProtocolViolation{Code: 400, Message: fmt.Sprintf("missing required field: %s", name), Reason: "protocolError"}
		}
		return nil
	}

	requireSupportedProtocolIfPresent := func() *ProtocolViolation {
		if _, ok := root["protocolVersion"]; ok && decoded.ProtocolVersion != SupportedProtocolVersion {
			return &ProtocolViolation{Code: 426, Message: fmt.Sprintf("unsupported protocolVersion: %d. Expected: %d", decoded.ProtocolVersion, SupportedProtocolVersion), Reason: "protocolError"}
		}
		return nil
	}

	switch intent {
	case "hello":
		if err := requireProtocol(); err != nil {
			return nil, err
		}
	case "createRoom":
		if err := requireSupportedProtocolIfPresent(); err != nil {
			return nil, err
		}
		if err := requireString("roomName", decoded.RoomName); err != nil {
			return nil, err
		}
		if _, ok := root["maxPlayers"]; !ok || decoded.MaxPlayers < 2 || decoded.MaxPlayers > 4 {
			return nil, &ProtocolViolation{Code: 400, Message: "missing or invalid required field: maxPlayers (must be 2-4)", Reason: "protocolError"}
		}
	case "joinRoom":
		if err := requireSupportedProtocolIfPresent(); err != nil {
			return nil, err
		}
		if err := requireString("roomId", decoded.RoomID); err != nil {
			return nil, err
		}
	case "leaveRoom":
		if err := requireSupportedProtocolIfPresent(); err != nil {
			return nil, err
		}
		if err := requireString("roomId", decoded.RoomID); err != nil {
			return nil, err
		}
	case "heartbeat":
		if err := requireSupportedProtocolIfPresent(); err != nil {
			return nil, err
		}
		if _, ok := root["heartbeatCounter"]; !ok {
			return nil, &ProtocolViolation{Code: 400, Message: "missing required field: heartbeatCounter", Reason: "protocolError"}
		}
	case "publishLinkEvent":
		if err := requireSupportedProtocolIfPresent(); err != nil {
			return nil, err
		}
		var evt LinkEvent
		if _, ok := root["event"]; !ok {
			return nil, &ProtocolViolation{Code: 400, Message: "missing required field: event", Reason: "protocolError"}
		}
		if err := json.Unmarshal(decoded.Event, &evt); err != nil {
			return nil, &ProtocolViolation{Code: 400, Message: "invalid event payload", Reason: "protocolError"}
		}
		if evt.Payload == "" {
			return nil, &ProtocolViolation{Code: 400, Message: "missing required field: event.payload", Reason: "protocolError"}
		}
		decodedPayload, err := base64.StdEncoding.DecodeString(evt.Payload)
		if err != nil {
			return nil, &ProtocolViolation{Code: 400, Message: "event.payload must be base64", Reason: "protocolError"}
		}
		if len(decodedPayload) > NetplayMaxLinkPayloadBytes {
			return nil, &ProtocolViolation{Code: 413, Message: "event.payload exceeds NETPLAY_MAX_LINK_PAYLOAD_BYTES", Reason: "protocolError"}
		}
	default:
		return nil, &ProtocolViolation{Code: 400, Message: fmt.Sprintf("unknown intent: %s", intent), Reason: "protocolError"}
	}

	if _, ok := root["clientSequence"]; !ok {
		return nil, &ProtocolViolation{Code: 400, Message: "missing required field: clientSequence", Reason: "protocolError"}
	}

	return &decoded, nil
}

func MarshalEvent(kind string, fields map[string]any) ([]byte, error) {
	if fields == nil {
		fields = map[string]any{}
	}
	fields["kind"] = kind
	fields["protocolVersion"] = SupportedProtocolVersion
	b, err := json.Marshal(fields)
	if err != nil {
		return nil, fmt.Errorf("marshal event: %w", err)
	}
	return b, nil
}
