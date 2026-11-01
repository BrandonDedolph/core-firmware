// Package player is core-app's preview listen: play one album track at a
// time through the computer's speakers, in pure Go.
//
// The audio device is github.com/ebitengine/oto/v3, opened once per
// process at 44.1 kHz, stereo, int16 (OutRate). FLAC is decoded by
// github.com/mewkiz/flac and MP3 by github.com/hajimehoshi/go-mp3; a track
// at another rate is resampled linearly on the way, mono is played on both
// channels and 24-bit audio is dithered to 16. This is a listen before a
// sync, not a mastering chain.
//
// Built with -tags noaudio the device half is a stub: Available reports
// false and Play returns ErrUnavailable. The tests use that build, so no
// test ever opens an audio device.
package player

import (
	"errors"
	"fmt"
	"io"
	"os"
	"sync"
	"sync/atomic"
	"time"
)

// The errors a caller can tell apart.
var (
	// ErrUnavailable is a build or a machine with no audio output.
	ErrUnavailable = errors.New("playback is not available in this build")
	// ErrUnsupported is a file this package does not decode.
	ErrUnsupported = errors.New("not a format this player decodes")
	// ErrMissing is a file that is not there (any more): a Fix renamed
	// it, or it was deleted. It wraps os.ErrNotExist.
	ErrMissing = fmt.Errorf("the file is not there: %w", os.ErrNotExist)
	// ErrNoSeek is a Seek with nothing to seek in.
	ErrNoSeek = errors.New("this track cannot seek")
)

// State is what the player is doing.
type State int

// The states.
const (
	Stopped State = iota
	Playing
	Paused
)

// String names the state for a log line.
func (s State) String() string {
	switch s {
	case Playing:
		return "playing"
	case Paused:
		return "paused"
	default:
		return "stopped"
	}
}

// device is the process's audio output; voice is one stream on it. The oto
// build implements them (oto.go); the noaudio build has none; a test has
// its own.
type device interface {
	newVoice(r io.Reader) voice
}

type voice interface {
	Play()
	Pause()
	IsPlaying() bool
	// BufferedSize is the bytes read from the stream not yet played.
	BufferedSize() int
	// SetVolume scales this stream's samples, 0..1 (oto's Player does it
	// in its mixer, on the running stream).
	SetVolume(v float64)
	Err() error
	Close() error
}

// Player plays one track at a time. All of its methods are safe to call
// from any goroutine; the UI calls them from its own.
type Player struct {
	open func() (device, error)

	mu     sync.Mutex
	dev    device
	devErr error
	cur    *session
	state  State
	// volume is this app's level, 0..1: a scale on its own output, not
	// the system's volume. Every new stream starts at it.
	volume float64
	// noRG turns ReplayGain off (it is on unless SetReplayGain(false)).
	// Read by the decoder per chunk, so a toggle is heard at once.
	noRG    atomic.Bool
	onEnd   func()
	onError func(error)
}

// New is a player on this build's audio output. Nothing is opened until the
// first Play.
func New() *Player { return &Player{open: openDevice, volume: 1} }

// newWith is a player on a given device, for the tests.
func newWith(d device) *Player {
	return &Player{open: func() (device, error) { return d, nil }, volume: 1}
}

// OnEnd sets what runs when a track plays to its end (not when it is
// stopped). It runs on the player's own goroutine.
func (p *Player) OnEnd(f func()) {
	p.mu.Lock()
	p.onEnd = f
	p.mu.Unlock()
}

// OnError sets what runs when a track fails part way (a decode error). It
// runs on the player's own goroutine; the track is stopped.
func (p *Player) OnError(f func(error)) {
	p.mu.Lock()
	p.onError = f
	p.mu.Unlock()
}

// Play starts path from the beginning, stopping whatever was playing. The
// errors it can see at once — no audio, no file, a format it does not
// decode — it returns; a file that fails to decode reaches OnError.
func (p *Player) Play(path string) error { return p.start(path, 0, Playing) }

// start is Play and Seek: a fresh session on path at offset, in state st.
func (p *Player) start(path string, at time.Duration, st State) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.dev == nil && p.devErr == nil {
		p.dev, p.devErr = p.open()
	}
	if p.devErr != nil {
		return p.devErr
	}
	if err := checkPath(path); err != nil {
		return err
	}
	p.stopLocked()
	s := newSession(p, path, at)
	p.cur, p.state = s, st
	s.gain = trackGain(path)
	s.voice = p.dev.newVoice(s)
	// A track change — or a seek, which is a fresh stream — keeps the level.
	s.voice.SetVolume(p.volume)
	go s.decode()
	go s.watch()
	return nil
}

// SetVolume sets this app's playback level, 0 (silent) to 1 (as decoded),
// on the stream that is playing and every one after it. It is not the
// system's volume: it scales this app's output only. Works with no device
// open (the noaudio build keeps the number).
func (p *Player) SetVolume(v float64) {
	if v < 0 {
		v = 0
	} else if v > 1 {
		v = 1
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	p.volume = v
	if p.cur != nil {
		p.cur.voice.SetVolume(v)
	}
}

// SetReplayGain turns ReplayGain on (the default) or off, on the playing
// track at once and on every track after it.
func (p *Player) SetReplayGain(on bool) { p.noRG.Store(!on) }

// ReplayGain reports whether it is on.
func (p *Player) ReplayGain() bool { return !p.noRG.Load() }

// TrackGain is the ReplayGain of the loaded track — what its tags ask for
// and what the peak cap lets through — whether or not it is switched on.
// The zero Gain when nothing is loaded or the track has no tag.
func (p *Player) TrackGain() Gain {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.cur == nil {
		return Gain{}
	}
	return p.cur.gain
}

// Volume is the level SetVolume last set (1 until then).
func (p *Player) Volume() float64 {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.volume
}

// Pause holds the track where it is.
func (p *Player) Pause() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.cur != nil && p.state == Playing {
		p.state = Paused
		p.cur.voice.Pause()
	}
}

// Resume carries on from a Pause.
func (p *Player) Resume() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.cur != nil && p.state == Paused {
		p.state = Playing
		if p.cur.ready.Load() {
			p.cur.voice.Play()
		}
	}
}

// Stop ends the track. OnEnd does not run.
func (p *Player) Stop() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.stopLocked()
}

func (p *Player) stopLocked() {
	if p.cur != nil {
		p.cur.close()
		p.cur = nil
	}
	p.state = Stopped
}

// Seek moves the current track to d, keeping it playing or paused. It is a
// fresh decode from d: the device's buffer is dropped with the old stream.
func (p *Player) Seek(d time.Duration) error {
	p.mu.Lock()
	s, st := p.cur, p.state
	p.mu.Unlock()
	if s == nil {
		return ErrNoSeek
	}
	if d < 0 {
		d = 0
	}
	if t := s.length(); t > 0 && d >= t {
		d = t - time.Second
	}
	return p.start(s.path, d, st)
}

// Position is how far into the track the listener is, and its length (0
// while the length is not known yet).
func (p *Player) Position() (pos, total time.Duration) {
	p.mu.Lock()
	s := p.cur
	p.mu.Unlock()
	if s == nil {
		return 0, 0
	}
	return s.position(), s.length()
}

// State is what the player is doing.
func (p *Player) State() State {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.state
}

// ended is the watch goroutine's report: s has played out, or failed.
func (p *Player) ended(s *session, err error) {
	p.mu.Lock()
	if p.cur != s {
		p.mu.Unlock()
		return
	}
	p.stopLocked()
	onEnd, onError := p.onEnd, p.onError
	p.mu.Unlock()
	if err != nil {
		if onError != nil {
			onError(err)
		}
		return
	}
	if onEnd != nil {
		onEnd()
	}
}

// playing reports whether s is the current session and meant to be heard.
func (p *Player) playing(s *session) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.cur == s && p.state == Playing
}

// --- one track ------------------------------------------------------------

// chunkFrames is how much is decoded at a time; queueChunks is how many
// decoded chunks wait for the device (about 1.5 s); prefill is how many are
// ready before the device starts reading, so its first read does not wait
// on the decoder.
const (
	chunkFrames = 4096
	queueChunks = 16
	prefill     = 4
	// readWait bounds how long the device's read waits for the decoder.
	// The device holds its own lock while it reads, so a read that
	// blocked on a stalled decoder would stall Pause and Position too.
	readWait = 20 * time.Millisecond
)

// session is one track being played: the decoder goroutine feeding chunks
// of output-format PCM, the io.Reader the device pulls them through, and
// the counters Position is made of.
type session struct {
	p     *Player
	path  string
	base  time.Duration
	voice voice
	// gain is the track's ReplayGain (unity when it has none).
	gain Gain

	chunks chan []byte
	quit   chan struct{}
	once   sync.Once
	cur    []byte

	pushed atomic.Int64 // bytes handed to the device
	eof    atomic.Bool  // the reader has returned io.EOF
	ready  atomic.Bool  // prefilled; the device may start
	total  atomic.Int64 // track length in output frames, 0 unknown
	err    atomic.Value // a decode error, as errBox
}

type errBox struct{ err error }

func newSession(p *Player, path string, at time.Duration) *session {
	return &session{
		p: p, path: path, base: at,
		chunks: make(chan []byte, queueChunks),
		quit:   make(chan struct{}),
	}
}

// Read is the device's end of the queue.
func (s *session) Read(b []byte) (int, error) {
	if len(s.cur) == 0 {
		t := time.NewTimer(readWait)
		defer t.Stop()
		select {
		case c, ok := <-s.chunks:
			if !ok {
				s.eof.Store(true)
				return 0, io.EOF
			}
			s.cur = c
		case <-s.quit:
			s.eof.Store(true)
			return 0, io.EOF
		case <-t.C:
			return 0, nil
		}
	}
	n := copy(b, s.cur)
	s.cur = s.cur[n:]
	s.pushed.Add(int64(n))
	return n, nil
}

// decode runs the source into the queue until the end, an error or close.
// It starts the device once the queue is prefilled, if the player is
// still meant to be playing this session (a paused Seek leaves that to
// Resume).
func (s *session) decode() {
	start := func() {
		if s.ready.Swap(true) {
			return
		}
		if s.p.playing(s) {
			// On its own goroutine: the device's Play reads from this
			// queue until its buffer is full, and this goroutine is the
			// one that fills the queue.
			go func() {
				s.voice.Play()
				// A Pause that landed between the check and the
				// Play wins.
				if !s.p.playing(s) {
					s.voice.Pause()
				}
			}()
		}
	}
	// Deferred in this order so the queue is closed — the reader sees
	// the end — before a short track that never reached the prefill is
	// started.
	defer start()
	defer close(s.chunks)
	src, err := openSource(s.path)
	if err != nil {
		s.err.Store(errBox{err})
		return
	}
	defer src.close()
	rate, ch, bits := src.format()
	if rate <= 0 || ch <= 0 {
		s.err.Store(errBox{ErrUnsupported})
		return
	}
	if n := src.total(); n > 0 {
		s.total.Store(n * OutRate / int64(rate))
	}
	if s.base > 0 {
		if err := src.seek(int64(s.base.Seconds() * float64(rate))); err != nil {
			s.err.Store(errBox{err})
			return
		}
	}
	conv := newConverter(ch, bits)
	rs := newResampler(rate)
	var pending []byte
	sent := 0
	for {
		in, err := src.next()
		if len(in) > 0 {
			st := conv.stereo16(in)
			// ReplayGain on the 16-bit samples before anything else, as the
			// firmware's apply_gain_s16 does; the user's volume is oto's,
			// applied after.
			if s.gain.Have && !s.p.noRG.Load() {
				applyGain(st, s.gain.q12)
			}
			pending = append(pending, le16(rs.process(st))...)
		}
		for len(pending) >= chunkFrames*bytesPerFrame || (err != nil && len(pending) > 0) {
			n := chunkFrames * bytesPerFrame
			if n > len(pending) {
				n = len(pending)
			}
			c := make([]byte, n)
			copy(c, pending[:n])
			pending = pending[n:]
			select {
			case s.chunks <- c:
			case <-s.quit:
				return
			}
			if sent++; sent == prefill {
				start()
			}
		}
		if err == io.EOF {
			return
		}
		if err != nil {
			s.err.Store(errBox{err})
			return
		}
	}
}

// watch notices the end of the track — the reader has seen EOF and the
// device has played what it held — or a failure, and tells the player.
func (s *session) watch() {
	t := time.NewTicker(50 * time.Millisecond)
	defer t.Stop()
	for {
		select {
		case <-s.quit:
			return
		case <-t.C:
		}
		if b, ok := s.err.Load().(errBox); ok && b.err != nil {
			s.p.ended(s, b.err)
			return
		}
		if err := s.voice.Err(); err != nil {
			s.p.ended(s, err)
			return
		}
		if s.eof.Load() && !s.voice.IsPlaying() && s.p.playing(s) {
			s.p.ended(s, nil)
			return
		}
	}
}

// position is base plus what the device has actually played.
func (s *session) position() time.Duration {
	played := s.pushed.Load() - int64(s.voice.BufferedSize())
	if played < 0 {
		played = 0
	}
	return s.base + time.Duration(played/bytesPerFrame)*time.Second/OutRate
}

func (s *session) length() time.Duration {
	return time.Duration(s.total.Load()) * time.Second / OutRate
}

func (s *session) close() {
	s.once.Do(func() {
		close(s.quit)
		_ = s.voice.Close()
	})
}
