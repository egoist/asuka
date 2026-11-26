package devserver

// The performance tools' side of the connection (core/src/abi/dev.h's
// frames 24 to 27, core/src/ui/perf.c): the overlay a player shows over
// the app (frames a second, flushes, the last flush's phases, memory),
// and recordings: each flush's phases and work, the display's frames and
// those dropped, and the app's functions sampled, which a player sends
// as a Profile when the recording stops (profile.go turns it into a
// trace for Perfetto and chrome://tracing, or a speedscope profile).

import (
	"encoding/binary"
	"time"
)

// Performance frame kinds (core/src/abi/dev.h).
const (
	// KindPerf turns the performance overlay on or off: u8 flags (1: on).
	KindPerf = 24
	// KindProfileStart starts a recording: u8 flags (1: sample the app's
	// functions), u32 the sampling interval in microseconds (0: 1 ms).
	KindProfileStart = 25
	// KindProfileStop ends it: the player answers with KindProfile.
	KindProfileStop = 26
	// KindProfile is a recording, as JSON (Profile).
	KindProfile = 27
)

const (
	perfOverlay = 1
	profileCPU  = 1
)

// SetPerfOverlay turns the player's performance overlay on or off.
func (p *Player) SetPerfOverlay(on bool) error {
	var f byte
	if on {
		f = perfOverlay
	}
	p.wmu.Lock()
	p.perf = on
	p.wmu.Unlock()
	return p.send(KindPerf, []byte{f})
}

// PerfOverlay reports whether the player's overlay is on, as asuka dev
// last turned it.
func (p *Player) PerfOverlay() bool {
	p.wmu.Lock()
	defer p.wmu.Unlock()
	return p.perf
}

// StartProfile starts a recording on the player; cpu samples the app's
// functions every interval (0: 1 ms) too.
func (p *Player) StartProfile(cpu bool, interval time.Duration) error {
	b := []byte{0}
	if cpu {
		b[0] = profileCPU
	}
	b = binary.LittleEndian.AppendUint32(b, uint32(interval/time.Microsecond))
	p.wmu.Lock()
	p.recording = true
	p.wmu.Unlock()
	return p.send(KindProfileStart, b)
}

// StopProfile ends the player's recording: Events.Profile hears it.
func (p *Player) StopProfile() error {
	p.wmu.Lock()
	p.recording = false
	p.wmu.Unlock()
	return p.send(KindProfileStop, nil)
}

// Recording reports whether a recording asuka dev started on the player
// runs.
func (p *Player) Recording() bool {
	p.wmu.Lock()
	defer p.wmu.Unlock()
	return p.recording
}

// SetPerfOverlay turns every player's overlay on or off, and that of
// each that says hello from now on.
func (s *Server) SetPerfOverlay(on bool) {
	s.mu.Lock()
	s.perf = on
	var ps []*Player
	for p := range s.ps {
		if p.hello {
			ps = append(ps, p)
		}
	}
	s.mu.Unlock()
	for _, p := range ps {
		p.SetPerfOverlay(on)
	}
}

// PerfOverlay reports whether the players' overlays are on, as
// Server.SetPerfOverlay last turned them.
func (s *Server) PerfOverlay() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.perf
}

// Ready are the players that said hello, in the order they connected.
func (s *Server) Ready() []*Player {
	s.mu.Lock()
	defer s.mu.Unlock()
	var ps []*Player
	for p := range s.ps {
		if p.hello {
			ps = append(ps, p)
		}
	}
	for i := 1; i < len(ps); i++ {
		for j := i; j > 0 && ps[j].seq < ps[j-1].seq; j-- {
			ps[j], ps[j-1] = ps[j-1], ps[j]
		}
	}
	return ps
}
