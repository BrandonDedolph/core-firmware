//go:build !noaudio

package player

import (
	"fmt"
	"io"
	"sync"
	"time"

	"github.com/ebitengine/oto/v3"
)

// Available reports whether this build can play audio at all.
const Available = true

// The one oto context. oto refuses a second NewContext in a process, so a
// failed open is remembered rather than retried.
var (
	otoOnce sync.Once
	otoCtx  *oto.Context
	otoErr  error
)

// openDevice opens the process's audio output, once.
func openDevice() (device, error) {
	otoOnce.Do(func() {
		ctx, ready, err := oto.NewContext(&oto.NewContextOptions{
			SampleRate:   OutRate,
			ChannelCount: OutChannels,
			Format:       oto.FormatSignedInt16LE,
		})
		if err != nil {
			otoErr = fmt.Errorf("%w: %v", ErrUnavailable, err)
			return
		}
		select {
		case <-ready:
		case <-time.After(5 * time.Second):
			otoErr = fmt.Errorf("%w: the audio device did not open", ErrUnavailable)
			return
		}
		otoCtx = ctx
	})
	if otoErr != nil {
		return nil, otoErr
	}
	return otoDevice{otoCtx}, nil
}

type otoDevice struct{ ctx *oto.Context }

func (d otoDevice) newVoice(r io.Reader) voice {
	p := d.ctx.NewPlayer(r)
	// A quarter of a second: enough that a busy machine does not crackle,
	// little enough that Pause and Seek answer at once.
	p.SetBufferSize(OutRate / 4 * bytesPerFrame)
	return p
}
