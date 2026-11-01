package app

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// The gates: what stands between a click and a destructive write.
//
// A write to the firmware partition (Flash, Update, Install) is confirmed in
// one dialog that says WHICH iPod, in words a person can check against the
// thing in their hand — name, model, serial, which disk on this PC, how much
// is written — and then asks for a deliberate gesture: a two-second
// press-and-hold. The hold IS the confirmation: the app answers the
// flasher's typed-path prompt with the path it already identified — after
// checking that the flasher is asking about that same path. Hardware Core
// has not been tested on is not written from the app at all (ErrUntested).
//
// Sync and remove lists the albums it would delete and says how many on its
// button. No typed word: the list is the confirmation.

// HoldDuration is how long the write button must be held.
const HoldDuration = 2 * time.Second

// writeGate is what a write job reads from the model on the UI goroutine,
// before it starts, for the dialog it will raise on its own goroutine.
type writeGate struct {
	title string
	dev   Device
	cli   string
	image string
}

// gate snapshots the model for one write.
func (u *UI) gate(title string) writeGate {
	return writeGate{title: title, dev: u.st.Device, cli: u.st.CLIPath}
}

// cardRow is one label/value line of the identity card (or one row of the
// removal list or the sync preview, where mid is the muted middle column:
// "9 of 12 tracks").
type cardRow struct{ label, mid, value string }

// identityCard is the five lines that say which iPod this is.
func identityCard(d Device, imageBytes int64) []cardRow {
	name := strings.TrimSpace(d.Name)
	if name == "" {
		name = modelWords(d.Model)
	}
	writes := "the firmware image"
	if imageBytes > 0 {
		v, unit := humanBinary(imageBytes)
		writes = v + " " + unit + " of firmware"
	}
	rows := []cardRow{
		{label: "Name", value: name},
		{label: "Model", value: modelWords(d.Model)},
	}
	if d.Serial != "" {
		rows = append(rows, cardRow{label: "Serial", value: d.Serial})
	}
	rows = append(rows,
		cardRow{label: "Disk", value: diskLine(d)},
		cardRow{label: "Writes", value: writes + " · the partition is backed up first and read back after"},
	)
	return rows
}

// physicalDriveRE is Windows' disk number in a \\.\PhysicalDriveN path.
var physicalDriveRE = regexp.MustCompile(`(?i)PhysicalDrive(\d+)$`)

// diskLine is "Disk 2 (as Disk Management numbers it) · 74.5 GB" on Windows
// — PhysicalDriveN is 0-based, the same number Disk Management shows, so
// that is the name a person can check it against — and the device path
// elsewhere (/dev/sdb, /dev/disk4).
func diskLine(d Device) string {
	size := ""
	if d.Size > 0 {
		v, unit := humanBinary(d.Size)
		size = " · " + v + " " + unit
	}
	if m := physicalDriveRE.FindStringSubmatch(d.Path); m != nil {
		return "Disk " + m[1] + " (as Disk Management numbers it)" + size
	}
	if d.Path != "" {
		return d.Path + size
	}
	return "this disk" + size
}

// writeNote is the sentence under the card.
const writeNote = "Nothing on the music volume changes. If anything goes wrong, hold Select+Menu, " +
	"then Select+Play at the Apple logo: that is disk mode, it is in ROM, and this window " +
	"will find the iPod again."

// confirmWrite is the flasher's Confirm for one write. It runs on the job
// goroutine; everything it needs from the model is in g.
func (u *UI) confirmWrite(ctx context.Context, plan *strings.Builder, g writeGate) func(prompt string) (string, error) {
	return func(prompt string) (string, error) {
		want, ok := devicePathFromPrompt(prompt)
		if !ok {
			// The flasher's wording changed under us. Refusing here is
			// the only honest move: a dialog that cannot say what to
			// type would either wave the write through or fail it
			// with a message about a path nobody was shown.
			return "", fmt.Errorf("the flasher's confirmation prompt is not in the form core-app knows: %q", prompt)
		}
		var size int64
		if fi, err := os.Stat(g.image); err == nil {
			size = fi.Size()
		}
		req, err := writeDialog(g, want, size, strings.TrimSpace(tail(plan.String(), 60)))
		if err != nil {
			return "", err
		}
		answer, err := u.ask(ctx, req)
		if err == nil && answer == CancelAnswer {
			return "", errors.New("cancelled: nothing was written")
		}
		return answer, err
	}
}

// ErrUntested is the app's answer to a firmware write on hardware Core has
// not been tested on. The flasher refuses such a write before it asks
// (flasher.gateHardware) unless --untested-hardware is given, and the app
// never gives it: the command line is the one place to take that risk.
var ErrUntested = errors.New("this iPod is not hardware Core has been tested on, so the app will not " +
	"write firmware to it — the command line can, with --untested-hardware " +
	"(core flash --untested-hardware <image>, or core install --untested-hardware)")

// writeDialog is the request confirmWrite raises: the identity card, the
// note, the plan behind its link, and the gate — the two-second hold,
// which answers with the path this window identified. It is refused
// outright if the flasher is asking about a different path, and for
// hardware Core has not been tested on (ErrUntested).
func writeDialog(g writeGate, want string, size int64, plan string) (*dialogRequest, error) {
	if !g.dev.Tested {
		return nil, ErrUntested
	}
	if want != g.dev.Path {
		return nil, fmt.Errorf("the flasher asked to write %s, but this window identified the iPod as %s — refusing",
			want, g.dev.Path)
	}
	req := &dialogRequest{
		title:      g.title,
		card:       identityCard(g.dev, size),
		note:       writeNote,
		plan:       plan,
		hold:       true,
		holdAnswer: g.dev.Path,
		okLabel:    "Hold to write…",
	}
	if runtime.GOOS == "windows" && !g.dev.Elevated && g.cli != "" {
		req.note += " Windows will ask for permission to run " + fileBase(g.cli) + ", which does the write."
	}
	return req, nil
}

// holdTick advances the hold-to-write button by one frame. pressed is
// whether the pointer is down on it now; it returns how far the fill has
// grown (0..1) and whether this frame completed the hold. Releasing early
// starts again from nothing.
func (u *UI) holdTick(pressed bool, now time.Time) (float32, bool) {
	if !pressed {
		u.holdStart = time.Time{}
		return 0, false
	}
	if u.holdStart.IsZero() {
		u.holdStart = now
	}
	f := float32(now.Sub(u.holdStart)) / float32(HoldDuration)
	if f >= 1 {
		u.holdStart = time.Time{}
		return 1, true
	}
	return f, false
}

// removalDialog is Sync and remove's second step: what would be deleted, one
// row per path the prune will remove, labelled by what it is — an album
// folder, a file inside an album that stays, or a playlist — with its size,
// the total, and a button that counts what it deletes: "Delete 3 albums and
// sync" when every entry is an album folder, "Delete 4 items and sync"
// otherwise. info reads one path's size and whether it is a directory
// (pathInfo; a test passes its own).
func removalDialog(plan *syncer.Plan, info func(string) (int64, bool)) *dialogRequest {
	const maxRows = 12
	var rows []cardRow
	albums := true
	for i, p := range plan.Prune {
		size, dir := info(p)
		label, isAlbum := pruneLabel(p, dir)
		if !isAlbum {
			albums = false
		}
		if i >= maxRows {
			continue
		}
		v := ""
		if size > 0 {
			n, unit := humanBinary(size)
			v = n + " " + unit
		}
		rows = append(rows, cardRow{label: label, value: v})
	}
	if n := len(plan.Prune); n > maxRows {
		rows = append(rows, cardRow{label: fmt.Sprintf("… and %d more", n-maxRows)})
	}
	n := len(plan.Prune)
	noun := fmt.Sprintf("%d item%s", n, plural(n))
	if albums {
		noun = fmt.Sprintf("%d album%s", n, plural(n))
	}
	total := noun
	if plan.PruneBytes > 0 {
		v, unit := humanBinary(plan.PruneBytes)
		total += " · " + v + " " + unit
	}
	return &dialogRequest{
		title:   "Delete these from the iPod?",
		items:   rows,
		total:   total + " will be deleted from the iPod",
		note:    "These are on the iPod but not in your music folder. The sync runs right after.",
		okLabel: "Delete " + noun + " and sync",
	}
}

// pruneLabel says what one pruned path is, relative to Music: an album
// folder (the name itself), a file inside an album that stays ("01.flac,
// in Album"), a playlist ("x.m3u8, a playlist"), or a loose file in Music.
// isAlbum is true only for an album folder.
func pruneLabel(p string, dir bool) (label string, isAlbum bool) {
	parts := strings.FieldsFunc(filepath.ToSlash(p), func(r rune) bool { return r == '/' || r == '\\' })
	rel := parts
	for i, s := range parts {
		if strings.EqualFold(s, "Music") {
			rel = parts[i+1:]
		}
	}
	switch {
	case len(rel) == 0:
		return p, false
	case len(rel) >= 2 && strings.EqualFold(rel[0], devicefs.PlaylistDir):
		return rel[len(rel)-1] + ", a playlist", false
	case len(rel) == 1 && dir:
		return rel[0], true
	case len(rel) == 1:
		return rel[0] + ", a file in Music", false
	default:
		return rel[len(rel)-1] + ", in " + rel[0], false
	}
}

// pathInfo is a pruned path's size (a directory's files, as the syncer
// counts them) and whether it is a directory.
func pathInfo(p string) (int64, bool) {
	fi, err := os.Stat(p)
	if err != nil {
		return 0, false
	}
	if fi.IsDir() {
		return syncer.TreeSize(p), true
	}
	return fi.Size(), false
}
