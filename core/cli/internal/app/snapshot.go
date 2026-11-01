//go:build !linux || novulkan

package app

import (
	"context"
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"time"

	"gioui.org/gpu/headless"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/unit"
)

// Snapshot renders one frame of the given State at w×h into a PNG and
// returns.
//
// This is the review channel. The orchestrator building this app cannot
// see the user's desktop, and neither can CI; a window that has never
// been looked at is a window whose text is clipped in a way nobody
// notices until it ships. `core-app --screenshot out.png --state demo`
// is the same call, so the picture a human reviews and the picture the
// test asserts on come out of the same code path.
//
// It renders through gioui.org/gpu/headless, which on this WSL box goes
// to Mesa's software EGL (a DRI3 warning on stderr is normal). Where
// there is no EGL at all, headless.NewWindow fails and the caller
// (TestSnapshot) skips rather than fails: a machine with no GPU stack
// is not a broken layout.
//
// Snapshot draws in Linen whatever the machine's own setting is, so the
// same test renders the same picture on every box; SnapshotTheme picks.
func Snapshot(st State, w, h int, out string) error {
	return snapshotWith(st, w, h, out, nil)
}

// SnapshotTheme is Snapshot in a chosen palette (`--theme`). System
// resolves the way the window would on this machine.
func SnapshotTheme(st State, t Theme, w, h int, out string) error {
	return snapshotThemed(st, t, w, h, out, nil)
}

// snapshotWith is Snapshot with a hook that runs after the State is set
// and before the frame is laid out, for the test that needs a modal on
// screen.
// SnapshotDialog is SnapshotTheme with one of DialogKinds open.
func SnapshotDialog(st State, t Theme, kind string, w, h int, out string) error {
	var derr error
	err := snapshotThemed(st, t, w, h, out, func(u *UI) { derr = cannedDialog(u, kind) })
	if derr != nil {
		return derr
	}
	return err
}

func snapshotWith(st State, w, h int, out string, tweak func(*UI)) error {
	return snapshotThemed(st, ThemeLinen, w, h, out, tweak)
}

// snapshotWait is how long a screenshot waits for its covers to decode.
const snapshotWait = 10 * time.Second

// settle is the screenshot's barrier. The first layout asks for every
// picture the frame shows — the grid's visible tiles, the sheet's cover,
// the footer's — and starts their decodes; settle waits for all of them
// (and for a sheet's tracklist, if one is being read), then lays the frame
// out again into the same ops, which is the frame that is saved. A layout
// that asks for more pictures than the last one (a cover the tracklist
// brought into view) goes round again. The window never does this: there
// a cover arrives on a later frame.
func (u *UI) settle(gtx layout.Context, limit time.Duration) error {
	ctx, cancel := context.WithTimeout(context.Background(), limit)
	defer cancel()
	caches := []*thumbCache{u.thumbs, u.lowCovers, u.artThumbs}
	loads := func() int {
		n := 0
		for _, c := range caches {
			n += c.Loads()
		}
		return n
	}
	for {
		before := loads()
		for _, c := range caches {
			if err := c.Wait(ctx); err != nil {
				return fmt.Errorf("screenshot: %w", err)
			}
		}
		for u.sheet != nil && !u.sheet.loaded {
			u.mu.Lock()
			landed := u.newTracks != nil
			u.mu.Unlock()
			if landed {
				break
			}
			select {
			case <-ctx.Done():
				return fmt.Errorf("screenshot: the album's tracks are still being read: %w", ctx.Err())
			case <-time.After(5 * time.Millisecond):
			}
		}
		gtx.Ops.Reset()
		u.Layout(gtx)
		if loads() == before {
			return nil
		}
	}
}

func snapshotThemed(st State, t Theme, w, h int, out string, tweak func(*UI)) error {
	hw, err := headless.NewWindow(w, h)
	if err != nil {
		return fmt.Errorf("%w: %v", ErrNoHeadless, err)
	}
	defer hw.Release()

	ui := NewUI(Options{Theme: &t})
	// No Backend here, so the grid's covers come straight from the album
	// folders: a canned state's folders do not exist and every tile is its
	// letter plate, a real library (--source) draws its own covers.
	ui.thumbs = folderThumbs(ui.invalidate)
	ui.SetState(st)
	if tweak != nil {
		tweak(ui)
	}

	var ops op.Ops
	sz := image.Pt(w, h)
	gtx := layout.Context{
		Ops:         &ops,
		Metric:      unit.Metric{PxPerDp: 1, PxPerSp: 1},
		Constraints: layout.Exact(sz),
	}
	ui.Layout(gtx)
	if err := ui.settle(gtx, snapshotWait); err != nil {
		return err
	}
	if err := hw.Frame(&ops); err != nil {
		return fmt.Errorf("rendering the frame: %w", err)
	}
	img := image.NewRGBA(image.Rectangle{Max: sz})
	if err := hw.Screenshot(img); err != nil {
		return fmt.Errorf("reading the frame back: %w", err)
	}
	if dir := filepath.Dir(out); dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	f, err := os.Create(out)
	if err != nil {
		return err
	}
	if err := png.Encode(f, img); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}
