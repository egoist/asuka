package dap

// The Debug Adapter Protocol's wire: messages as JSON, each after a
// Content-Length header (https://microsoft.github.io/debug-adapter-protocol/),
// and the parts of its types the adapter reads and writes.

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// message is a request the editor sends (or a response or event, which
// it doesn't).
type message struct {
	Seq       int             `json:"seq"`
	Type      string          `json:"type"`
	Command   string          `json:"command"`
	Arguments json.RawMessage `json:"arguments"`
}

type response struct {
	Seq        int    `json:"seq"`
	Type       string `json:"type"` // "response"
	RequestSeq int    `json:"request_seq"`
	Success    bool   `json:"success"`
	Command    string `json:"command"`
	Message    string `json:"message,omitempty"`
	Body       any    `json:"body,omitempty"`
}

type event struct {
	Seq   int    `json:"seq"`
	Type  string `json:"type"` // "event"
	Event string `json:"event"`
	Body  any    `json:"body,omitempty"`
}

// maxMessage is the most a message may be: an editor's are small.
const maxMessage = 64 << 20

// readMessage reads a message's header and its JSON.
func readMessage(r *bufio.Reader) ([]byte, error) {
	n := -1
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return nil, err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			if n < 0 {
				continue // between messages
			}
			break
		}
		k, v, ok := strings.Cut(line, ":")
		if ok && strings.EqualFold(strings.TrimSpace(k), "Content-Length") {
			if n, err = strconv.Atoi(strings.TrimSpace(v)); err != nil || n < 0 || n > maxMessage {
				return nil, fmt.Errorf("dap: a bad Content-Length: %q", v)
			}
		}
	}
	b := make([]byte, n)
	if _, err := io.ReadFull(r, b); err != nil {
		if errors.Is(err, io.EOF) {
			err = io.ErrUnexpectedEOF
		}
		return nil, err
	}
	return b, nil
}

// writeMessage writes v as a message.
func writeMessage(w io.Writer, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	_, err = w.Write(append([]byte("Content-Length: "+strconv.Itoa(len(b))+"\r\n\r\n"), b...))
	return err
}

// The requests' arguments the adapter reads.

type initializeArgs struct {
	ClientID        string `json:"clientID"`
	AdapterID       string `json:"adapterID"`
	LinesStartAt1   *bool  `json:"linesStartAt1"`
	ColumnsStartAt1 *bool  `json:"columnsStartAt1"`
	PathFormat      string `json:"pathFormat"`
}

type source struct {
	Name             string `json:"name,omitempty"`
	Path             string `json:"path,omitempty"`
	PresentationHint string `json:"presentationHint,omitempty"`
	Origin           string `json:"origin,omitempty"`
}

type setBreakpointsArgs struct {
	Source      source `json:"source"`
	Breakpoints []struct {
		Line int `json:"line"`
	} `json:"breakpoints"`
	Lines []int `json:"lines"` // deprecated, an older client's
}

type threadArgs struct {
	ThreadID int `json:"threadId"`
}

type stackTraceArgs struct {
	ThreadID   int `json:"threadId"`
	StartFrame int `json:"startFrame"`
	Levels     int `json:"levels"`
}

type scopesArgs struct {
	FrameID int `json:"frameId"`
}

type variablesArgs struct {
	VariablesReference int    `json:"variablesReference"`
	Filter             string `json:"filter"`
	Start              int    `json:"start"`
	Count              int    `json:"count"`
}

// The bodies it writes.

// capabilities are what the adapter does, as initialize answers.
type capabilities struct {
	SupportsConfigurationDoneRequest   bool  `json:"supportsConfigurationDoneRequest"`
	SupportsFunctionBreakpoints        bool  `json:"supportsFunctionBreakpoints"`
	SupportsConditionalBreakpoints     bool  `json:"supportsConditionalBreakpoints"`
	SupportsHitConditionalBreakpoints  bool  `json:"supportsHitConditionalBreakpoints"`
	SupportsEvaluateForHovers          bool  `json:"supportsEvaluateForHovers"`
	ExceptionBreakpointFilters         []any `json:"exceptionBreakpointFilters"`
	SupportsStepBack                   bool  `json:"supportsStepBack"`
	SupportsSetVariable                bool  `json:"supportsSetVariable"`
	SupportsRestartFrame               bool  `json:"supportsRestartFrame"`
	SupportsGotoTargetsRequest         bool  `json:"supportsGotoTargetsRequest"`
	SupportsStepInTargetsRequest       bool  `json:"supportsStepInTargetsRequest"`
	SupportsCompletionsRequest         bool  `json:"supportsCompletionsRequest"`
	SupportsModulesRequest             bool  `json:"supportsModulesRequest"`
	SupportsRestartRequest             bool  `json:"supportsRestartRequest"`
	SupportsExceptionOptions           bool  `json:"supportsExceptionOptions"`
	SupportsValueFormattingOptions     bool  `json:"supportsValueFormattingOptions"`
	SupportsExceptionInfoRequest       bool  `json:"supportsExceptionInfoRequest"`
	SupportTerminateDebuggee           bool  `json:"supportTerminateDebuggee"`
	SupportsDelayedStackTraceLoading   bool  `json:"supportsDelayedStackTraceLoading"`
	SupportsLoadedSourcesRequest       bool  `json:"supportsLoadedSourcesRequest"`
	SupportsLogPoints                  bool  `json:"supportsLogPoints"`
	SupportsTerminateThreadsRequest    bool  `json:"supportsTerminateThreadsRequest"`
	SupportsSetExpression              bool  `json:"supportsSetExpression"`
	SupportsTerminateRequest           bool  `json:"supportsTerminateRequest"`
	SupportsDataBreakpoints            bool  `json:"supportsDataBreakpoints"`
	SupportsReadMemoryRequest          bool  `json:"supportsReadMemoryRequest"`
	SupportsDisassembleRequest         bool  `json:"supportsDisassembleRequest"`
	SupportsCancelRequest              bool  `json:"supportsCancelRequest"`
	SupportsBreakpointLocationsRequest bool  `json:"supportsBreakpointLocationsRequest"`
	SupportsSteppingGranularity        bool  `json:"supportsSteppingGranularity"`
	SupportsInstructionBreakpoints     bool  `json:"supportsInstructionBreakpoints"`
	SupportsSingleThreadExecutionReqs  bool  `json:"supportsSingleThreadExecutionRequests"`
}

type breakpoint struct {
	ID       int     `json:"id"`
	Verified bool    `json:"verified"`
	Message  string  `json:"message,omitempty"`
	Source   *source `json:"source,omitempty"`
	Line     int     `json:"line,omitempty"`
	Reason   string  `json:"reason,omitempty"` // "pending": not in the app's code yet
}

type dapThread struct {
	ID   int    `json:"id"`
	Name string `json:"name"`
}

type stackFrame struct {
	ID               int     `json:"id"`
	Name             string  `json:"name"`
	Source           *source `json:"source,omitempty"`
	Line             int     `json:"line"`
	Column           int     `json:"column"`
	PresentationHint string  `json:"presentationHint,omitempty"`
}

type scope struct {
	Name               string `json:"name"`
	PresentationHint   string `json:"presentationHint,omitempty"`
	VariablesReference int    `json:"variablesReference"`
	NamedVariables     int    `json:"namedVariables,omitempty"`
	Expensive          bool   `json:"expensive"`
}

type variable struct {
	Name               string `json:"name"`
	Value              string `json:"value"`
	Type               string `json:"type,omitempty"`
	VariablesReference int    `json:"variablesReference"`
	NamedVariables     int    `json:"namedVariables,omitempty"`
	IndexedVariables   int    `json:"indexedVariables,omitempty"`
}
