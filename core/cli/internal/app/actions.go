package app

import (
	"context"
	"fmt"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/ghrelease"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/installer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
)

// This file is what the buttons do: one function per job, each handed
// to the Runner, each written so that everything it needs from the
// model is read on the UI goroutine BEFORE the closure is built. A job
// goroutine that reached back into u.st would be the one data race in
// the program, and it would be invisible until a sync collided with a
// repaint.
//
// Results come back the other way: a job writes u.newDev / u.newLib /
// u.newRel under the mutex and the next frame's drain() folds them in.

// --- the jobs ----------------------------------------------------------

func (u *UI) start(kind JobKind, fn func(ctx context.Context, emit func(Event)) error) {
	if err := u.run.Start(kind, fn); err != nil {
		u.st.Logf("%s: %v", kind, err)
		return
	}
	u.st.Job = &JobStatus{Kind: kind, Pct: -1}
	u.st.Notice = ""
	u.st.Logf("--- %s ---", kind)
}

func (u *UI) startRefresh() {
	u.start(JobRefresh, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventProgress, Text: "looking for an iPod", Pct: -1})
		dev, lib, err := u.be.Refresh(ctx)
		if err != nil {
			return err
		}
		u.mu.Lock()
		u.newDev, u.newLib = &dev, &lib
		u.mu.Unlock()
		if dev.Found {
			u.readBackups(ctx, dev.Serial)
			emit(Event{Kind: EventLog, Text: fmt.Sprintf("device: %s — %s, %s",
				dev.Path, dev.Model, disk.HumanSize(dev.Size))})
			if dev.Installed.Description != "" {
				emit(Event{Kind: EventLog, Text: "firmware: " + dev.Installed.Description})
			}
			if dev.Installed.Kind == fwpart.Other {
				// The Install screen says this in plain words; the
				// file names and the paths are the log's.
				emit(Event{Kind: EventLog, Text: "install: Apple's firmware is saved first, as " +
					appleBackupName(dev.Serial) + "; then the image is written and read back, and " +
					"CORECFG.DAT, CORELOG.BIN and Music\\ are created on " + dev.Volume +
					" (the firmware can overwrite those files but cannot create them)"})
			}
		} else {
			emit(Event{Kind: EventLog, Text: "no iPod found: " + dev.Err})
		}
		return nil
	})
}

// startSync runs one of the three Music buttons.
//
// Sync + prune is two dialogs and two passes on purpose: the first
// dialog says what prune means, then a DRY RUN produces the actual
// orphan list, and the second dialog shows that list and asks for a
// typed word. Nothing is deleted on the strength of a number nobody has
// seen.
func (u *UI) startSync(kind JobKind) {
	src := strings.TrimSpace(u.sourceEd.Text())
	dst := u.st.Device.Volume
	tooOld := u.st.FirmwareTooOld()
	u.start(kind, func(ctx context.Context, emit func(Event)) error {
		if src == "" {
			return fmt.Errorf("no music folder: choose it on the Fix tab")
		}
		if dst == "" {
			return fmt.Errorf("no iPod volume: plug the iPod in (disk mode: Select+Play) — this window finds it on its own")
		}
		if tooOld && kind != JobDryRun {
			return fmt.Errorf("%s", TooOldText)
		}
		o := syncer.Options{Src: src, Dst: dst, GenreMap: library.GenreMapEmbedded}
		switch kind {
		case JobDryRun:
			o.DryRun = true
			plan, err := u.be.Sync(ctx, o, emit)
			u.publishPlan(plan, false)
			return err
		case JobSync:
			plan, err := u.be.Sync(ctx, o, emit)
			u.publishPlan(plan, err == nil)
			return err
		}

		// JobSyncPrune.
		answer, err := u.ask(ctx, &dialogRequest{
			title: "Sync and remove deletes music",
			body: "Sync and remove deletes every album on the iPod that your music folder " +
				"(" + sourceName(src) + ") does not have, with its covers. A preview comes next " +
				"and lists exactly what would go.",
			prose:   true,
			okLabel: "Show me the list",
		})
		if err != nil {
			return err
		}
		if answer == "" {
			return fmt.Errorf("cancelled before the preview")
		}

		dry := o
		dry.DryRun, dry.Prune = true, true
		plan, err := u.be.Sync(ctx, dry, emit)
		if err != nil {
			return err
		}
		if len(plan.Prune) == 0 {
			emit(Event{Kind: EventLog, Text: "nothing to remove; running a plain sync instead"})
			_, err := u.be.Sync(ctx, o, emit)
			return err
		}
		for _, p := range plan.Prune {
			emit(Event{Kind: EventLog, Text: "would remove " + p})
		}
		answer, err = u.ask(ctx, removalDialog(plan, pathInfo))
		if err != nil {
			return err
		}
		if answer == "" {
			return fmt.Errorf("cancelled: nothing was deleted")
		}
		o.Prune, o.Yes = true, true
		plan, err = u.be.Sync(ctx, o, emit)
		u.publishPlan(plan, err == nil)
		return err
	})
}

// startSyncPreview is the sync a person starts: the header's "Sync N
// albums" and the iPod tab's "Preview sync". It plans first (a dry run,
// which also refreshes the grid), shows what the plan would copy
// (previewDialog), and copies only when that sheet's button is pressed.
// Cancel — or Close on "Everything is on the iPod" — ends the job with
// nothing written. The plan's lines go to the log as well.
func (u *UI) startSyncPreview() {
	src := strings.TrimSpace(u.sourceEd.Text())
	dst := u.st.Device.Volume
	tooOld := u.st.FirmwareTooOld()
	u.start(JobSync, func(ctx context.Context, emit func(Event)) error {
		if src == "" {
			return fmt.Errorf("no music folder: choose it on the Fix tab")
		}
		if dst == "" {
			return fmt.Errorf("no iPod volume: plug the iPod in (disk mode: Select+Play) — this window finds it on its own")
		}
		o := syncer.Options{Src: src, Dst: dst, GenreMap: library.GenreMapEmbedded}
		dry := o
		dry.DryRun = true
		emit(Event{Kind: EventProgress, Text: "working out what to copy", Pct: -1})
		plan, err := u.be.Sync(ctx, dry, emit)
		u.publishPlan(plan, false)
		if err != nil {
			return err
		}
		albums := AlbumsFromPlan(plan)
		for _, a := range albums {
			if a.Copy > 0 {
				emit(Event{Kind: EventLog, Text: fmt.Sprintf("would copy %s — %d of %d tracks, %s",
					a.Title, a.Copy, a.Tracks, disk.HumanSize(a.Bytes))})
			}
		}
		req := previewDialog(albums, src, tooOld)
		answer, err := u.ask(ctx, req)
		if err != nil {
			return err
		}
		if req.only || answer == CancelAnswer {
			emit(Event{Kind: EventLog, Text: "preview only: nothing was copied"})
			return nil
		}
		plan, err = u.be.Sync(ctx, o, emit)
		u.publishPlan(plan, err == nil)
		return err
	})
}

// startCheck asks GitHub for the latest release. It runs once by itself
// after the first Refresh that finds an iPod (see afterJob) and again from
// the menu. A failure is a line in the log and nothing else: the header is
// not the place to say that a network request the user did not make went
// wrong, so the job itself ends cleanly and the header simply shows no
// update state.
func (u *UI) startCheck() { u.check(false) }

// startLaunchCheck is startCheck plus the quiet second question, asked once
// per launch: is there a newer desktop app? It says nothing unless there is
// (About and the ··· menu then offer it) or the question fails (a log line).
func (u *UI) startLaunchCheck() { u.check(true) }

func (u *UI) check(withApp bool) {
	running := version.Version
	u.start(JobCheck, func(ctx context.Context, emit func(Event)) error {
		if withApp {
			defer u.checkApp(ctx, running, emit)
		}
		emit(Event{Kind: EventProgress, Text: "asking GitHub", Pct: -1})
		rel, err := u.be.Release(ctx)
		if err != nil {
			rel.Checked, rel.Err = true, err.Error()
		}
		u.mu.Lock()
		u.newRel = &rel
		u.mu.Unlock()
		if err != nil {
			emit(Event{Kind: EventLog, Text: "could not check for updates: " + firstLine(err.Error())})
			return nil
		}
		emit(Event{Kind: EventLog, Text: "latest release: " + rel.Tag + " (" + rel.Asset + ")"})
		if rel.Notes != "" {
			emit(Event{Kind: EventLog, Text: "--- release notes ---\n" + rel.Notes + "\n--- end ---"})
		}
		return nil
	})
}

func (u *UI) startUpdate() {
	rel := u.st.Release
	installed := installedTag(u.st.Device.Firmware)
	g := u.gate("Update this iPod to Core " + rel.Tag + "?")
	u.start(JobUpdate, func(ctx context.Context, emit func(Event)) error {
		if !g.dev.Tested {
			return ErrUntested
		}
		if !rel.Checked || rel.Tag == "" {
			return fmt.Errorf("no release known yet: use ··· → Check for updates first")
		}
		if installed != "" && installed == rel.Tag {
			// Not a refusal — `core update --tag` re-flashes on purpose
			// too — but a fact the user should read before the dialog.
			emit(Event{Kind: EventLog, Text: "the iPod already runs " + installed +
				"; continuing will write the same version again"})
		}
		file, err := u.be.Download(ctx, rel, emit)
		if err != nil {
			return err
		}
		g.image = file
		if err := u.flashFile(ctx, file, g, emit); err != nil {
			return err
		}
		return u.readBack(ctx, emit)
	})
}

// checkApp is the app half of the launch check. Job goroutine.
func (u *UI) checkApp(ctx context.Context, running string, emit func(Event)) {
	tag, url, err := u.be.AppRelease(ctx)
	if err != nil {
		emit(Event{Kind: EventLog, Text: "could not check for a newer app: " + firstLine(err.Error())})
		return
	}
	if !ghrelease.AppNewer(tag, running) {
		return
	}
	u.mu.Lock()
	u.newApp = &[2]string{tag, url}
	u.mu.Unlock()
}

// installedTag is the "v0.1.2" out of fwpart.VersionText's
// "v0.1.2 (build …)", or "" when the text does not start with a tag.
func installedTag(firmware string) string {
	tag, _, _ := strings.Cut(strings.TrimSpace(firmware), " ")
	if strings.HasPrefix(tag, "v") {
		return tag
	}
	return ""
}

func (u *UI) startFlash(file string) {
	g := u.gate("Write " + fileBase(file) + " to this iPod?")
	g.image = file
	u.start(JobFlash, func(ctx context.Context, emit func(Event)) error {
		if file == "" {
			return fmt.Errorf("no image: type a path to a .ipod or .bin file, or use ··· → Flash an image file…")
		}
		if !g.dev.Tested {
			return ErrUntested
		}
		if err := u.flashFile(ctx, file, g, emit); err != nil {
			return err
		}
		return u.readBack(ctx, emit)
	})
}

// readBack is the one tail every write shares — Flash, Update and Install:
// read the device again and hand the result to the window, so the header
// says what is on the iPod NOW. Before it, a flash returned straight after
// the write and the window kept the old version until a replug (the detect
// poll keys on path and serial, which a flash does not change).
//
// It runs on the job goroutine and publishes under the mutex like every
// other job result.
func (u *UI) readBack(ctx context.Context, emit func(Event)) error {
	dev, lib, err := u.be.Refresh(ctx)
	if err != nil {
		return err
	}
	u.mu.Lock()
	u.newDev, u.newLib = &dev, &lib
	u.mu.Unlock()
	if dev.Found {
		u.readBackups(ctx, dev.Serial)
	}
	now := dev.Installed.Description
	if now == "" {
		now = dev.Firmware
	}
	if now != "" {
		emit(Event{Kind: EventLog, Text: "firmware now: " + now})
	}
	return nil
}

// flashFile is the shared tail of Flash and Update: the write, behind the
// gate (confirmWrite), with the flasher's own plan going to the log and to
// the dialog's "Show the plan".
func (u *UI) flashFile(ctx context.Context, file string, g writeGate, emit func(Event)) error {
	plan, tee := teePlan(emit)
	return u.be.Flash(ctx, file, u.confirmWrite(ctx, plan, g), tee)
}

// teePlan splits a job's log events two ways: on to the window, and
// into a buffer the confirmation dialog shows as the plan. Everything
// the flasher printed before it asked is what the person is agreeing
// to, and it is the flasher's own words rather than a paraphrase.
func teePlan(emit func(Event)) (*strings.Builder, func(Event)) {
	plan := &strings.Builder{}
	return plan, func(e Event) {
		if e.Kind == EventLog {
			plan.WriteString(e.Text)
		}
		emit(e)
	}
}

// startInstall is the Install screen's one button: put Core on an iPod
// that is still running Apple's firmware.
//
// file is an image the user picked, or "" for the latest release —
// which is downloaded and checksum-verified here, in this process,
// before anything is written, so the elevated child (when there is one)
// never makes a network request.
//
// It ends with a Refresh rather than with what it wrote: the phase the
// window moves to is decided by reading the device back, so a write
// that verified but somehow left something else installed cannot show
// the main screen.
func (u *UI) startInstall(file string) {
	rel := u.st.Release
	g := u.gate("")
	u.start(JobInstall, func(ctx context.Context, emit func(Event)) error {
		if !g.dev.Tested {
			return ErrUntested
		}
		image := file
		if image == "" {
			if !rel.Checked || rel.Tag == "" {
				got, err := u.be.Release(ctx)
				u.mu.Lock()
				u.newRel = &got
				u.mu.Unlock()
				if err != nil {
					return err
				}
				rel = got
			}
			var err error
			if image, err = u.be.Download(ctx, rel, emit); err != nil {
				return err
			}
		}

		g.image = image
		if file != "" {
			g.title = "Install Core from " + fileBase(file) + " on this iPod?"
		} else {
			g.title = "Install Core " + rel.Tag + " on this iPod?"
		}
		plan, tee := teePlan(emit)
		confirm := u.confirmWrite(ctx, plan, g)
		res, err := u.be.Install(ctx, installer.Options{Image: image}, tee, confirm)
		if err != nil {
			return err
		}
		if res != nil && res.Flash != nil && res.Flash.Aborted {
			return fmt.Errorf("aborted: the confirmation did not match")
		}
		if res != nil && res.Flash != nil {
			if apple := res.Flash.AppleBackup(); apple != "" {
				emit(Event{Kind: EventLog, Text: "Apple's firmware is kept at " + apple +
					" — `core flash --from-backup " + apple + "` puts it back"})
			}
		}

		return u.readBack(ctx, emit)
	})
}

// devicePathFromPrompt pulls the exact string the flasher wants typed
// out of its own prompt, so the dialog and the flasher agree about what
// counts as confirmation without this file re-deriving the device path.
// The prompt's shape is owned by internal/flasher (ConfirmPrompt); this
// is its inverse.
func devicePathFromPrompt(prompt string) (string, bool) {
	return flasher.ConfirmTarget(prompt)
}

func (u *UI) startBackup() {
	serial := u.st.Device.Serial
	u.start(JobBackup, func(ctx context.Context, emit func(Event)) error {
		path, err := u.be.Backup(ctx, emit)
		if err != nil {
			return err
		}
		emit(Event{Kind: EventLog, Text: "backup written: " + path})
		u.mu.Lock()
		u.newNotice = "Backed up to " + path
		u.mu.Unlock()
		u.readBackups(ctx, serial)
		return nil
	})
}

// readBackups asks the backend what the backup folder holds for this iPod
// and hands it to the window. Job goroutine; publishes under the mutex.
func (u *UI) readBackups(ctx context.Context, serial string) {
	apple, n, dir := u.be.Backups(ctx, serial)
	u.mu.Lock()
	u.newBackups = &Backups{Apple: apple, Count: n, Dir: dir}
	u.mu.Unlock()
}

func (u *UI) startEject() {
	vol := u.st.Device.Volume
	u.start(JobEject, func(ctx context.Context, emit func(Event)) error {
		if vol == "" {
			return fmt.Errorf("no volume to eject — plug the iPod in, or use ··· → Read the iPod again")
		}
		if err := u.be.Eject(ctx, vol, emit); err != nil {
			return err
		}
		// The strip shows the last log line; it should say the same thing
		// as the header.
		emit(Event{Kind: EventLog, Text: "ejected " + vol + " — safe to unplug"})
		return nil
	})
}

// pick opens the OS folder/file dialog on its own goroutine — the
// dialog is a separate process and can sit on screen for a minute, and
// a UI goroutine waiting on it is a frozen window.
func (u *UI) pick(for_ JobKind) {
	go func() {
		var (
			path string
			err  error
		)
		if for_ == JobSync {
			path, err = PickFolder(context.Background())
		} else {
			path, err = PickFile(context.Background())
		}
		if err != nil {
			u.Post(Event{Kind: EventLog, Text: "picker: " + err.Error()})
			return
		}
		if path == "" {
			return
		}
		u.mu.Lock()
		u.newPath, u.pathFor = path, for_
		u.mu.Unlock()
		u.invalidate()
	}()
}

// --- the library manager -----------------------------------------------

// The Library tab's four jobs. Each one reads what it needs from the
// model on the UI goroutine, hands the rest to the Runner, and writes
// its result back under the mutex for the next frame's drain() — the
// same shape every other job in this file has.

// doPrimary runs whatever the top bar's one button currently says.
//
// The decision is State.Primary and it is made in exactly one place, so
// the label and the action cannot disagree: a button that says Sync and
// ejects is the single worst bug a one-button window can have.
func (u *UI) doPrimary() {
	switch u.st.Primary() {
	case ActionInstall:
		u.startInstall("")
	case ActionSync:
		// Never straight to the copy: the preview says what will go.
		u.startSyncPreview()
	case ActionUpdate:
		u.startUpdate()
	case ActionEject:
		u.startEject()
	}
}

// setSource is the one place the music folder changes: the Browse
// button, the footer's Change, and Enter in either copy of the field.
//
// A new folder invalidates everything that was said about the old one —
// the report, the candidates, the decisions and the undo journal all
// describe a tree nobody is looking at any more — and then the two
// read-only passes run: the scan that fills the Library tab and the dry
// run that fills the grid.
func (u *UI) setSource(path string) {
	path = strings.TrimSpace(path)
	if path == "" {
		return
	}
	changed := path != u.st.Source
	u.st.Source = path
	u.sourceEd.SetText(path)
	u.saveConfig()
	if changed {
		u.st.Report, u.st.ArtCands, u.st.ArtChoices = nil, nil, nil
		u.st.Journal, u.st.Albums = "", nil
		u.st.LibraryChanged = false
	}
	u.startInspect(path)
	u.later(func() { u.startSync(JobDryRun) })
}

// startInspect is librarian.Inspect: one walk of the source tree, no
// network and no writes, which is why it runs on its own the moment a
// folder is chosen rather than waiting for a button.
func (u *UI) startInspect(src string) {
	src = strings.TrimSpace(src)
	if src == "" {
		u.st.Logf("scan: no music folder yet — choose one on the Fix tab")
		return
	}
	discs := u.st.DiscFolders
	u.start(JobInspect, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventProgress, Text: "reading the tags in " + src, Pct: -1})
		rep, err := u.be.Inspect(ctx, src, discs, emit)
		if err != nil {
			return err
		}
		u.mu.Lock()
		u.newReport = rep
		u.mu.Unlock()
		return nil
	})
}

// startCandidates is the one thing on this screen that goes to the
// internet, which is why it is a button and not part of the scan
// (plan §5 decision 2: Inspect never touches the network).
func (u *UI) startCandidates() {
	rep := u.st.Report
	if rep == nil || len(rep.MissingArt) == 0 {
		return
	}
	u.start(JobArt, func(ctx context.Context, emit func(Event)) error {
		cands, err := u.be.Candidates(ctx, rep, emit)
		if len(cands) > 0 {
			u.mu.Lock()
			u.newCands = cands
			u.mu.Unlock()
		}
		return err
	})
}

// startFix is Fix all: one job, one journal, one undo.
//
// The confirmation is not "are you sure" — it is the count of what will
// change and what that will cost the next sync, because a rename IS a
// re-copy and a person who has not been told that will read the sync
// that follows as a bug.
func (u *UI) startFix() {
	rep := u.st.Report
	if rep == nil {
		return
	}
	ch := u.choices()
	summary := u.fixSummary()
	src := u.st.Source
	u.start(JobFix, func(ctx context.Context, emit func(Event)) error {
		answer, err := u.ask(ctx, &dialogRequest{
			title:   "Fix the library?",
			body:    "in " + src + "\n\n" + summary,
			prompt:  "Names and folders can be undone until the next sync; a cover written into a file stays.",
			okLabel: "Fix all",
		})
		if err != nil {
			return err
		}
		if answer == "" {
			return fmt.Errorf("cancelled before anything was changed")
		}
		res, err := u.be.Fix(ctx, rep, ch, emit)
		if res != nil {
			u.mu.Lock()
			u.newFix = res
			u.mu.Unlock()
			emit(Event{Kind: EventLog, Text: fmt.Sprintf(
				"fixed: %d renamed, %d moved, %d cover(s) written, %d skipped",
				res.Renamed, res.Moved, res.ArtWritten, res.Skipped)})
			for _, f := range res.Failures {
				emit(Event{Kind: EventLog, Text: "could not finish " + string(f.Album) + ": " + f.Reason})
			}
			if res.Journal != "" {
				emit(Event{Kind: EventLog, Text: "undo journal: " + res.Journal})
			}
		}
		return err
	})
}

// startUndo replays the last journal backwards. It is enabled only while
// there is one: organizer refuses an op whose target changed since the
// move, so an undo of a journal the user has already synced past is not
// a promise this button should imply.
func (u *UI) startUndo(journal string) {
	if journal == "" {
		return
	}
	u.start(JobUndoFix, func(ctx context.Context, emit func(Event)) error {
		rep, err := u.be.UndoFix(ctx, journal, emit)
		if rep != nil {
			u.mu.Lock()
			u.newUndo = rep
			u.mu.Unlock()
			emit(Event{Kind: EventLog, Text: fmt.Sprintf("undone: %d file(s) put back, %d left alone",
				rep.Undone, len(rep.Skipped))})
			for _, sk := range rep.Skipped {
				emit(Event{Kind: EventLog, Text: "left alone: " + sk.Op.To + " — " + sk.Reason})
			}
			emit(Event{Kind: EventLog, Text: "embedded covers are not undone; " +
				"`core art --remove` takes a picture back out"})
		}
		return err
	})
}

// gridEvents is the Albums tab's half of the frame: a tile click opens the
// album sheet (album.go).
func (u *UI) gridEvents(gtx C, busy bool) {
	for i := range u.tileClicks {
		if i >= len(u.st.Albums) {
			break
		}
		if u.tileClicks[i].Clicked(gtx) {
			u.openSheet(u.st.Albums[i])
		}
	}
}

// later queues one thing to start on a frame when nothing is running.
//
// The Runner takes one job at a time on purpose, so "scan the library,
// then plan the sync" cannot be two Start calls in a row. This is the
// queue that makes a sequence out of them without the second one
// racing the first: drain() pops it when the model says idle.
func (u *UI) later(f func()) { u.pending = append(u.pending, f) }

// publishPlan hands a finished plan to the UI goroutine.
//
// synced says the plan was executed rather than only planned, which is
// the difference between "these albums are about to be copied" and
// "these albums have just been copied" — the same plan, two grids.
func (u *UI) publishPlan(plan *syncer.Plan, synced bool) {
	if plan == nil {
		return
	}
	u.mu.Lock()
	u.newPlan, u.planSynced = plan, synced
	u.mu.Unlock()
}
