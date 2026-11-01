package app

import (
	"context"
	"fmt"
	"image"
	"image/color"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"

	"gioui.org/font"
	"gioui.org/gesture"
	"gioui.org/io/key"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/text"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/player"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
)

// Shorthands. Gio code is 90 % these two types and the full names turn
// every layout function signature into two lines.
type (
	// C is layout.Context.
	C = layout.Context
	// D is layout.Dimensions.
	D = layout.Dimensions
)

// MinWidth and MinHeight are the window's minimum size. Below this the
// four cards and the log pane stop being readable at the same time,
// which is the whole layout.
const (
	MinWidth  = 720
	MinHeight = 520
)

// Options configure a UI.
type Options struct {
	// Backend is the wiring. Required.
	Backend Backend
	// Config is the loaded settings; ConfigPath is where changes go
	// back. An empty ConfigPath means "do not persist", which is what
	// the snapshot renderer and the tests want.
	Config     Config
	ConfigPath string
	// Source overrides Config.Source (the --source flag).
	Source string
	// Invalidate asks the window for a frame. nil in a headless
	// render, where there is exactly one frame and nothing to ask.
	Invalidate func()
	// NoDetect turns the 2-second poll off (--no-detect). The window
	// then only knows what a job told it, which is what a test or a
	// screenshot wants and nothing else does.
	NoDetect bool
	// LogFile is --log: a file the startup path appends to. A
	// `-H windowsgui` process has no console, so a window that fails to
	// open fails in complete silence; this is the only way to see why
	// from a shell.
	LogFile string
	// Audio is the album sheet's player; nil is internal/player's, which
	// opens nothing until the first track is played.
	Audio audio
	// Theme overrides Config.Theme for this run (the --theme flag)
	// without saving it. nil is "use the config", which is itself empty
	// — System — on a first run.
	Theme *Theme
}

// logf appends one line to Options.LogFile, if there is one.
func logf(o Options, format string, args ...any) {
	if o.LogFile == "" {
		return
	}
	f, err := os.OpenFile(o.LogFile, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, format+"\n", args...)
}

// dialogRequest is one modal question. It crosses a goroutine boundary:
// a job asks, the UI goroutine draws the dialog, and the answer goes
// back down reply. The job blocks in between, which is correct — a
// flash that proceeded while the confirmation was still on screen is
// the bug this whole mechanism exists to prevent.
type dialogRequest struct {
	title string
	// body is shown in a scrollable monospace block: the flasher's
	// plan, or the list of things a prune would delete.
	body string
	// prompt is the line above the input.
	prompt string
	// want is the exact text that must be typed. Empty means the
	// dialog is a plain Yes/Cancel.
	want string
	// okLabel is the confirming button's text.
	okLabel string
	// info marks a dialog that only says something (About): the body is
	// set in the text face rather than the plan's monospace, and there
	// is one button. prose is the text face with Cancel kept.
	info, prose bool
	// card is the write dialog's identity card; items and total are the
	// removal list; note is the Muted sentence under them; plan is the
	// flasher's plan behind "Show the plan".
	card, items []cardRow
	total       string
	note, plan  string
	// only marks a dialog with one button, which closes it: it says
	// something and asks nothing ("Everything is on the iPod"). Such a
	// dialog has no Cancel, and Esc or a click on the scrim closes it.
	only bool
	// hold makes the confirming button a press-and-hold (HoldDuration)
	// that answers holdAnswer when it completes.
	hold       bool
	holdAnswer string
	// reply carries the typed answer, or "" for a cancel. Buffered, so
	// a job that has already given up cannot wedge the UI goroutine.
	reply chan string
}

// solo reports whether the dialog has one button and nothing to cancel.
func (d *dialogRequest) solo() bool { return d.info || d.only }

// renameResult is what a finished rename job leaves for the next frame:
// the friendly name that was asked for, the label the volume actually
// took (read back), and the serial to file the name under.
type renameResult struct{ friendly, label, serial string }

// UI is the window's state: the model, the widgets, and the queues the
// job goroutines talk to it through.
type UI struct {
	th  *material.Theme
	pal Palette
	// theme is the choice (System, Linen, Onyx); pal is what it
	// resolved to. applyTheme changes both.
	theme Theme
	st    State
	run   *Runner
	be    Backend

	cfg        Config
	cfgPath    string
	invalidate func()

	// Everything below the mutex is written by job goroutines and read
	// (and cleared) by the UI goroutine at the top of a frame. State
	// itself is never in here — that is the rule.
	mu        sync.Mutex
	inbox     []Event
	askQueue  *dialogRequest
	newDev    *Device
	newLib    *Library
	newRel    *Release
	newPath   string // a path chosen in the folder/file picker
	pathFor   JobKind
	newName   *renameResult // what a finished rename settled on
	newDetect *detectNews   // what the last poll found (detect.go)
	// The library manager's four results, plus the plan the grid is
	// drawn from.
	newReport  *librarian.Report
	newCands   map[librarian.AlbumKey][]artfetch.Candidate
	newFix     *librarian.Result
	newUndo    *organizer.UndoReport
	newPlan    *syncer.Plan
	planSynced bool

	dlg *dialogRequest // the dialog on screen; UI goroutine only
	// sheet is the album sheet (album.go), nil when closed; audio is the
	// player. newTracks,
	// trackDone and trackErr are the sheet's results from other
	// goroutines (ReadTracks, the player's OnEnd and OnError), under mu.
	sheet     *albumSheet
	audio     audio
	newTracks *tracksResult
	trackDone bool
	trackErr  error
	// npLive says State.NowPlaying is the player's (a canned state's is
	// a picture with no player behind it); npWarn is "could not play",
	// up for warnFor after npWarnAt. The footer bar's buttons follow.
	npLive                                      bool
	npWarn                                      string
	npWarnAt                                    time.Time
	npPlayBtn, npNextBtn, npStopBtn, npTitleBtn widget.Clickable
	npCoverBtn                                  widget.Clickable
	npSeek                                      gesture.Click
	npBarW                                      int

	// pending is the one-job-at-a-time queue: things to start on a
	// frame where nothing is running (see later()). UI goroutine only.
	pending []func()

	// det is the auto-detect poll's memory of the last set of devices
	// it saw. It has its own mutex; see detect.go.
	det detector

	cards   widget.List
	logList widget.List
	dlgBody widget.List
	// grid, libList and prevList are the three scrolling panes the tabs
	// add: the album grid, the Library tab's problem column and its
	// preview.
	grid     widget.List
	libList  widget.List
	prevList widget.List

	// thumbs is the album grid's cover cache; artThumbs is the same
	// machinery keyed by URL for the art candidates in the Library tab.
	thumbs    *thumbCache
	artThumbs *thumbCache

	sourceEd  widget.Editor
	flashEd   widget.Editor
	confirmEd widget.Editor
	nameEd    widget.Editor

	// editingName is the header's click-to-edit state: the name line
	// is a label until it is clicked, then a field with Save/Cancel.
	// It is UI-goroutine-only, like everything else outside the mutex.
	editingName bool

	browseBtn                                   widget.Clickable
	dryRunBtn, pruneBtn                         widget.Clickable
	checkBtn, updateBtn, flashBtn, flashOpenBtn widget.Clickable
	backupBtn, ejectBtn, copyBtn                widget.Clickable
	nameClick, nameSaveBtn, nameCancelBtn       widget.Clickable
	installBtn, installFileBtn, installPickBtn  widget.Clickable
	dlgOK, dlgCancel                            widget.Clickable
	// dlgScrim is the click outside a dialog; dlgBlock stops a click on
	// the plate from being one. itemList scrolls a dialog's list.
	dlgScrim, dlgBlock widget.Clickable
	itemList           widget.List

	// The top bar's one primary button, the tabs, and the footer's
	// "Change" for the music folder.
	primaryBtn widget.Clickable
	tabClicks  [3]widget.Clickable
	// folderLink is the footer's music folder: it goes to the Library
	// tab and focuses the field there (focusSource asks the field to take
	// focus on the frame it is next drawn).
	folderLink   widget.Clickable
	focusSource  bool
	tileClicks   []*widget.Clickable
	catClicks    [5]widget.Clickable
	artTake      []*widget.Clickable
	artSkip      []*widget.Clickable
	fixAllBtn    widget.Clickable
	undoBtn      widget.Clickable
	rescanBtn    widget.Clickable
	artFindBtn   widget.Clickable
	namesTick    widget.Bool
	organizeTick widget.Bool
	discTick     widget.Bool

	// The header's own Cancel (the Details card keeps its own: one
	// Clickable drawn twice in a frame is a button that only works in
	// one of the two places), the "···" button and its menu.
	headCancelBtn widget.Clickable
	// The write dialog's hold button, its clock, and the plan toggle.
	// now is time.Now except in a test.
	dlgHold, planToggle widget.Clickable
	holdStart           time.Time
	holdFrac            float32
	planOpen            bool
	measureOps          op.Ops
	// holdFrozen pins the hold fill for a canned screenshot, which has
	// no pointer to hold it with. Zero everywhere else.
	holdFrozen float32
	// systemNow is what System resolves to, read once when the menu
	// opens (it is a registry read on Windows; not one per frame).
	systemNow Theme
	now       func() time.Time
	// The iPod tab's own: Show folder, and the Advanced disclosure.
	backupDirBtn widget.Clickable
	advancedBtn  widget.Clickable
	// The log strip and drawer (logstrip.go). footH is the footer's
	// height in the last frame — the drawer sits on it. logFrames counts
	// the drawer's layouts, for the test that it is not on the iPod tab.
	logStripBtn, logCloseBtn, clearLogBtn, drawerBlock widget.Clickable
	footH                                              int
	logFrames                                          int
	// newBackups is a job's read of the backup folder, for drain;
	// newNotice is a result for the header's line 2 (State.Notice).
	newBackups *Backups
	newNotice  string
	// newApp is the launch check's newer app release: tag, page URL.
	newApp      *[2]string
	menuBtn     widget.Clickable
	menuScrim   widget.Clickable
	menuClicks  [menuCount]widget.Clickable
	themeClicks [3]widget.Clickable
	// topH is the top bar's height in the last frame: where the menu
	// hangs from.
	topH int
	// autoChecked records that this launch has asked GitHub once by
	// itself; the menu's Check for updates asks again.
	autoChecked bool

	pickerOK bool
}

// NewUI builds the window's state.
func NewUI(o Options) *UI {
	// config.json is hand-editable: "dark" or "Light" are read the way the
	// flag reads them, and anything unknown is System, so a radio in the
	// menu is always on.
	choice, _ := ParseTheme(string(o.Config.Theme))
	if o.Theme != nil {
		choice = *o.Theme
	}
	th, pal := newTheme(choice)
	u := &UI{
		th:         th,
		pal:        pal,
		theme:      choice,
		run:        NewRunner(),
		be:         o.Backend,
		cfg:        o.Config,
		cfgPath:    o.ConfigPath,
		invalidate: o.Invalidate,
		pickerOK:   PickerAvailable(),
		now:        time.Now,
	}
	if u.invalidate == nil {
		u.invalidate = func() {}
	}
	u.cards.Axis = layout.Vertical
	u.grid.Axis = layout.Vertical
	u.libList.Axis = layout.Vertical
	u.prevList.Axis = layout.Vertical
	u.dlgBody.Axis = layout.Vertical
	u.logList.Axis = layout.Vertical
	u.itemList.Axis = layout.Vertical
	// The log is a transcript: the interesting end is the bottom, and a
	// pane that stays at the top while a sync scrolls past is a pane
	// nobody reads.
	u.logList.ScrollToEnd = true

	// The two picture caches. The album grid decodes the device's own
	// sidecars off the UI goroutine; the candidate row fetches 200 px
	// thumbnails through the Backend, so a test sees a fake and no
	// network.
	u.thumbs = newThumbCache(func(key string) (image.Image, error) {
		if u.be == nil {
			return nil, ErrNoThumb
		}
		dir, px := splitCoverKey(key)
		return u.be.Thumbnail(dir, px)
	}, u.invalidate)
	u.audio = o.Audio
	if u.audio == nil {
		u.audio = player.New()
	}
	u.audio.OnEnd(func() {
		u.mu.Lock()
		u.trackDone = true
		u.mu.Unlock()
		u.invalidate()
	})
	u.audio.OnError(func(err error) {
		u.mu.Lock()
		u.trackErr = err
		u.mu.Unlock()
		u.invalidate()
	})
	u.artThumbs = newThumbCache(func(url string) (image.Image, error) {
		if u.be == nil {
			return nil, ErrNoThumb
		}
		return u.be.FetchThumb(context.Background(), url)
	}, u.invalidate)

	// The two tick boxes the report's categories carry start ON: the
	// report only lists things that are wrong, and a Fix all that did
	// nothing because two boxes started empty is worse than one that
	// does what the list says. The multi-disc split starts OFF — it
	// changes the shape of an album rather than repairing it.
	u.st.FixNames, u.st.Organize = true, true
	u.namesTick.Value, u.organizeTick.Value = true, true

	u.sourceEd.SingleLine, u.sourceEd.Submit = true, true
	u.flashEd.SingleLine, u.flashEd.Submit = true, true
	u.confirmEd.SingleLine, u.confirmEd.Submit = true, true
	u.nameEd.SingleLine, u.nameEd.Submit = true, true

	src := o.Source
	if src == "" {
		src = o.Config.Source
	}
	u.st.Source = src
	u.sourceEd.SetText(src)
	u.st.CLIPath, _ = CLIPath()
	u.st.Logf("core-app %s", version.Version)
	if u.st.CLIPath != "" {
		u.st.Logf("core CLI: %s", u.st.CLIPath)
	} else {
		u.st.Logf("core CLI: not found beside this app — Flash and Update will print the command instead")
	}
	return u
}

// Runner exposes the job runner, for the window's pump goroutine.
func (u *UI) Runner() *Runner { return u.run }

// Post hands one event to the UI goroutine and asks for a frame. The
// window's pump calls it; so does anything else off the UI goroutine
// that has something to say.
func (u *UI) Post(e Event) {
	u.mu.Lock()
	u.inbox = append(u.inbox, e)
	u.mu.Unlock()
	u.invalidate()
}

// State returns a copy of the model, for the tests and the snapshot
// renderer.
func (u *UI) State() State { return u.st }

// SetState replaces the model wholesale. Only the canned states
// (Demo/Empty) and the tests use it.
//
// The two text fields are part of the model as far as a reader is
// concerned, but Gio keeps their contents inside the Editor widgets, so
// they have to be pushed across explicitly — otherwise a snapshot of
// the demo state renders with empty boxes and their hint text, which is
// exactly the screenshot that would make someone think the field does
// not work.
func (u *UI) SetState(st State) {
	u.st = st
	u.sourceEd.SetText(st.Source)
	u.flashEd.SetText(st.FlashFile)
	u.nameEd.SetText(st.Device.Name)
	u.namesTick.Value = st.FixNames
	u.organizeTick.Value = st.Organize
	u.discTick.Value = st.DiscFolders
	if st.Advanced {
		u.revealAdvanced()
	}
}

// --- the per-frame work ------------------------------------------------

// drain folds everything the job goroutines left behind into the model.
// It is the first thing a frame does, and the only place State changes
// from outside a click handler.
func (u *UI) drain() {
	u.mu.Lock()
	events := u.inbox
	u.inbox = nil
	dev, lib, rel := u.newDev, u.newLib, u.newRel
	u.newDev, u.newLib, u.newRel = nil, nil, nil
	backups := u.newBackups
	u.newBackups = nil
	notice := u.newNotice
	u.newNotice = ""
	newApp := u.newApp
	u.newApp = nil
	path, pathFor := u.newPath, u.pathFor
	u.newPath, u.pathFor = "", JobNone
	renamed := u.newName
	u.newName = nil
	detected := u.newDetect
	u.newDetect = nil
	report, cands := u.newReport, u.newCands
	u.newReport, u.newCands = nil, nil
	fixed, undone := u.newFix, u.newUndo
	u.newFix, u.newUndo = nil, nil
	plan, planSynced := u.newPlan, u.planSynced
	u.newPlan, u.planSynced = nil, false
	ask := u.askQueue
	u.askQueue = nil
	tracks, ended, playErr := u.newTracks, u.trackDone, u.trackErr
	u.newTracks, u.trackDone, u.trackErr = nil, false, nil
	u.mu.Unlock()

	if tracks != nil {
		u.applyTracks(tracks)
	}
	if playErr != nil {
		u.trackFailed(playErr)
	}
	if ended {
		u.trackEnded()
	}

	if dev != nil {
		u.st.Device = *dev
		u.st.Ejected = false
		// The label came off the volume; the friendly name is ours,
		// and this is the only place the two are joined.
		u.st.Device.Name = u.cfg.NameFor(dev.Serial)
		if !u.editingName {
			u.nameEd.SetText(u.st.Device.Name)
		}
	}
	if renamed != nil {
		u.st.Device.Name, u.st.Device.Label = renamed.friendly, renamed.label
		u.cfg.SetName(renamed.serial, renamed.friendly)
		u.saveConfig()
		u.nameEd.SetText(renamed.friendly)
	}
	if lib != nil {
		u.st.Library = *lib
	}
	if backups != nil {
		u.st.Backups = *backups
	}
	if newApp != nil {
		u.st.NewerApp, u.st.NewerAppURL = newApp[0], newApp[1]
	}
	if notice != "" {
		u.st.Notice, u.st.NoticeAt = notice, u.now()
	}
	if rel != nil {
		u.st.Release = *rel
		u.st.CheckedAt = time.Now()
	}
	// The events go after the results they announce: a job publishes its
	// result and THEN returns, so a Done and the device it read arrive in
	// the same drain, and afterJob has to see the new device — not the
	// one the job replaced — to decide what follows.
	for _, e := range events {
		u.st.Apply(e)
		u.afterJob(e)
	}
	switch {
	case path != "" && pathFor == JobSync:
		u.setSource(path)
	case path != "" && pathFor == JobFlash:
		// The menu's "Flash an image file…" fills the path and shows it
		// where the Flash button is. Picking a file is not consent to
		// write it.
		u.st.FlashFile = path
		u.flashEd.SetText(path)
		u.st.Tab = TabDetails
		u.st.Advanced = true
		u.revealAdvanced()
	case path != "" && pathFor == JobInstall:
		// Picking a file is choosing it, not installing it: the button
		// becomes "Install from <name>" and that click runs the install
		// through the same gate as Install Core.
		u.st.InstallFile = path
		u.st.Logf("install image: %s", path)
	}
	if report != nil {
		u.st.Report, u.st.Scanned = report, time.Now()
		u.logReport(report)
		// A rescan of the same tree (Rescan, or ticking the multi-disc
		// row) keeps the lookup and the Take/Skip decisions for every
		// album that is still coverless: the keys are the album folders,
		// and asking the internet and the person again for an answer they
		// already gave is the kind of thing that makes a rescan feel like
		// a punishment. A key that is gone (a cover arrived, the album
		// moved) is dropped with its decision.
		u.st.ArtCands, u.st.ArtChoices = keepArt(report, u.st.ArtCands, u.st.ArtChoices)
		if u.st.Category.Count(report) == 0 {
			u.st.Category = firstCategory(report)
		}
	}
	if cands != nil {
		u.st.ArtCands = cands
		// "Accept all at 0.9 or better" is the rule the CLI's --yes
		// uses and the one the design page names; applying it here
		// means the ordinary case needs no clicks at all, and every
		// row still says what it is taking and offers Skip.
		for key, dec := range librarian.AcceptAbove(cands, librarian.DefaultMinScore) {
			if dec.Skip {
				// AcceptAbove marks everything below the line as a skip.
				// A weak match is not a skip, it is a question: leave it
				// undecided so the row asks it.
				continue
			}
			u.setChoice(key, dec)
		}
		for _, key := range sortedKeys(cands) {
			if cs := cands[key]; len(cs) > 0 {
				u.st.Logf("cover art: %s — %.2f %s %s — %s", key, cs[0].Score, cs[0].Provider,
					cs[0].Artist, cs[0].Album)
			}
		}
		u.st.Logf("cover art: %d album(s) matched, %d accepted at %.2f or better",
			len(cands), len(u.st.ArtChoices), librarian.DefaultMinScore)
	}
	if fixed != nil {
		u.st.Journal = fixed.Journal
		if fixed.Renamed+fixed.Moved+fixed.ArtWritten > 0 {
			u.st.LibraryChanged = true
		}
		// The report describes a tree that no longer exists: scan
		// again, then re-plan the sync the renames just made bigger.
		src := u.st.Source
		u.later(func() { u.startInspect(src) })
		u.later(func() { u.startSync(JobDryRun) })
	}
	if undone != nil {
		u.st.Journal = ""
		u.st.LibraryChanged = false
		src := u.st.Source
		u.later(func() { u.startInspect(src) })
		u.later(func() { u.startSync(JobDryRun) })
	}
	if plan != nil {
		u.st.Albums = AlbumsFromPlan(plan)
		if planSynced {
			markSynced(u.st.Albums)
		}
		u.reindexNowPlaying()
	}
	if detected != nil {
		u.applyDetect(detected)
	}
	if ask != nil {
		u.dlg = ask
		u.confirmEd.SetText("")
	}
	u.runPending()
}

// afterJob is the one place a finished job schedules the next read-only
// pass. Nothing here writes anything: a refresh that found a volume is
// what fills the grid and the Library tab on launch, with no click.
func (u *UI) afterJob(e Event) {
	if e.Kind != EventDone {
		return
	}
	switch e.Job {
	case JobEject:
		u.st.Ejected = true
	case JobSync, JobSyncPrune:
		// The counts in the footer and the header's free space are the
		// volume's; a sync just changed both.
		u.later(u.startRefresh)
	case JobRefresh:
		// The two read-only passes over the music folder first: they fill
		// the grid and the Library tab, and the Runner takes one job at
		// a time, so a check queued ahead of them would hold the grid
		// empty for as long as GitHub takes to answer (30 s, black-holed).
		if src := u.st.Source; src != "" {
			if u.st.Report == nil {
				u.later(func() { u.startInspect(src) })
			}
			if len(u.st.Albums) == 0 && u.st.Device.Volume != "" {
				u.later(func() { u.startSync(JobDryRun) })
			}
		}
		// Then, once per launch, the first time an iPod is there: ask
		// GitHub whether there is something newer, so Update appears
		// without anybody pressing Check.
		if !u.autoChecked && u.st.Device.Found {
			u.autoChecked = true
			u.later(u.startLaunchCheck)
		}
	}
}

// runPending starts the next queued job, if the Runner is free. One per
// frame: the next one goes on the frame after that job's EventDone, which
// is also when the model is in a fit state to decide whether it is still
// wanted.
func (u *UI) runPending() {
	if len(u.pending) == 0 || u.st.Busy() || u.run.Busy() {
		return
	}
	f := u.pending[0]
	u.pending = u.pending[1:]
	f()
}

// firstCategory is the row the Library tab opens on: the first one with
// anything in it, so a report lands on something to look at.
func firstCategory(r *librarian.Report) Category {
	for _, c := range Categories {
		if c.Count(r) > 0 {
			return c
		}
	}
	return CatArt
}

// markSynced turns a plan that has just run into the grid it leaves
// behind: everything it was going to copy is on the iPod now.
func markSynced(albums []Album) {
	for i := range albums {
		albums[i].Copy = 0
		albums[i].Bytes = 0
		albums[i].State = TileOnDevice
	}
}

func (u *UI) saveConfig() {
	if u.cfgPath == "" {
		return
	}
	u.cfg.Source = u.st.Source
	if err := SaveConfig(u.cfgPath, u.cfg); err != nil {
		u.st.Logf("warning: could not save %s: %v", u.cfgPath, err)
	}
}

// Layout draws one frame. It is pure with respect to the outside world:
// everything it does is read the model, read widget events, and start
// jobs through the Runner.
func (u *UI) Layout(gtx C) D {
	u.drain()
	u.syncNowPlaying(gtx)
	u.playKeys(gtx)
	if u.st.NowPlaying != nil && u.dlg == nil {
		u.nowPlayingEvents(gtx)
	}
	switch {
	case u.dlg != nil:
		// A dialog replaces the menu that opened it, and sits over the
		// album sheet when a job asks while one is open.
		u.st.MenuOpen = false
		u.dialogEvents(gtx)
	case u.sheet != nil:
		u.st.MenuOpen = false
		u.sheetEvents(gtx)
	default:
		u.events(gtx)
	}

	if !u.st.MenuOpen {
		// The menu's "System — Onyx now" is read once per opening.
		u.systemNow = ""
	}
	paint.Fill(gtx.Ops, u.pal.Surface)
	return layout.Stack{}.Layout(gtx,
		layout.Stacked(func(gtx C) D {
			g := gtx
			if u.dlg != nil || u.sheet != nil {
				// A modal that does not actually block the buttons
				// behind it is a modal that lets somebody start a sync
				// while a flash confirmation is on screen.
				g = gtx.Disabled()
			}
			g.Constraints.Min = gtx.Constraints.Max
			return u.pageWithDrawer(g)
		}),
		layout.Expanded(func(gtx C) D {
			if u.sheet != nil {
				u.sheetLayer(gtx)
			}
			if u.dlg != nil {
				return u.dialog(gtx)
			}
			if u.st.MenuOpen {
				return u.menuLayer(gtx)
			}
			return D{Size: gtx.Constraints.Min}
		}),
	)
}

// pageWithDrawer is the page, and the log drawer over it when it is open:
// above the body, below the dialogs and the menu.
func (u *UI) pageWithDrawer(gtx C) D {
	return layout.Stack{}.Layout(gtx,
		layout.Stacked(u.page),
		layout.Expanded(func(gtx C) D {
			if !u.st.LogOpen {
				return D{}
			}
			return u.drawerLayer(gtx)
		}),
	)
}

// events turns this frame's clicks into jobs.
func (u *UI) events(gtx C) {
	busy := u.st.Busy()

	// The tabs answer whether a job is running or not: looking at the
	// log while a sync runs is exactly what the Details tab is for.
	for i, t := range []Tab{TabAlbums, TabLibrary, TabDetails} {
		if u.tabClicks[i].Clicked(gtx) {
			u.st.Tab = t
		}
	}
	u.libraryEvents(gtx, busy)
	u.gridEvents(gtx, busy)

	if !busy {
		if u.primaryBtn.Clicked(gtx) {
			u.doPrimary()
		}

		if u.installBtn.Clicked(gtx) {
			u.startInstall("")
		}
		if u.installFileBtn.Clicked(gtx) {
			if f := u.st.InstallFile; f != "" {
				u.startInstall(f)
			} else {
				u.pick(JobInstall)
			}
		}
		if u.installPickBtn.Clicked(gtx) {
			u.pick(JobInstall)
		}
		if u.browseBtn.Clicked(gtx) {
			u.chooseMusicFolder()
		}
		if u.flashOpenBtn.Clicked(gtx) {
			u.pick(JobFlash)
		}
		if u.dryRunBtn.Clicked(gtx) {
			// The plan, then the preview sheet — whose button syncs.
			u.startSyncPreview()
		}
		if u.pruneBtn.Clicked(gtx) {
			u.startSync(JobSyncPrune)
		}
		if u.checkBtn.Clicked(gtx) {
			u.startCheck()
		}
		if u.updateBtn.Clicked(gtx) {
			u.startUpdate()
		}
		if u.flashBtn.Clicked(gtx) {
			u.startFlash(strings.TrimSpace(u.flashEd.Text()))
		}
		if u.backupDirBtn.Clicked(gtx) {
			u.openBackupFolder()
		}
		if u.backupBtn.Clicked(gtx) {
			u.startBackup()
		}
		if u.ejectBtn.Clicked(gtx) {
			u.startEject()
		}
		if u.nameClick.Clicked(gtx) && u.st.Device.Found && !u.editingName {
			u.editingName = true
			u.nameEd.SetText(u.st.Device.Name)
			gtx.Execute(key.FocusCmd{Tag: &u.nameEd})
		}
	}
	u.nameEvents(gtx, busy)
	if u.headCancelBtn.Clicked(gtx) && u.st.CanCancel() {
		u.st.Logf("cancel requested")
		u.run.Cancel()
	}
	u.logEvents(gtx)
	if u.folderLink.Clicked(gtx) && u.st.Phase() == PhaseReady {
		u.st.Tab = TabLibrary
		u.focusSource = true
	}
	if u.advancedBtn.Clicked(gtx) {
		u.st.Advanced = !u.st.Advanced
		if u.st.Advanced {
			u.revealAdvanced()
		}
	}
	if u.menuBtn.Clicked(gtx) {
		u.st.MenuOpen = !u.st.MenuOpen
	}
	u.menuEvents(gtx)

	// The source field is saved as it is edited, not on some Save
	// button nobody would press.
	for {
		ev, ok := u.sourceEd.Update(gtx)
		if !ok {
			break
		}
		switch ev.(type) {
		case widget.ChangeEvent:
			if s := strings.TrimSpace(u.sourceEd.Text()); s != u.st.Source {
				u.st.Source = s
				u.saveConfig()
			}
		case widget.SubmitEvent:
			// Enter in the field is the gesture that means "this one":
			// the scan follows it, rather than following every
			// keystroke on the way to a path.
			u.setSource(strings.TrimSpace(u.sourceEd.Text()))
		}
	}
	for {
		ev, ok := u.flashEd.Update(gtx)
		if !ok {
			break
		}
		switch ev.(type) {
		case widget.ChangeEvent, widget.SubmitEvent:
			u.st.FlashFile = strings.TrimSpace(u.flashEd.Text())
		}
	}
}

// nameEvents is the click-to-edit field's half of the frame: Enter or
// Save applies, Cancel drops it, and a job running takes the edit away —
// a rename during a flash would be a rename of a dismounted volume.
//
// The editor is drained every frame, editing or not: a widget whose
// events nobody reads keeps them forever, and the Submit that arrived
// on the frame the job started would fire again on the frame it ended.
func (u *UI) nameEvents(gtx C, busy bool) {
	submitted := false
	for {
		ev, ok := u.nameEd.Update(gtx)
		if !ok {
			break
		}
		if _, isSubmit := ev.(widget.SubmitEvent); isSubmit {
			submitted = true
		}
	}
	if !u.editingName {
		return
	}
	if busy {
		u.editingName = false
		return
	}
	if u.nameCancelBtn.Clicked(gtx) {
		u.editingName = false
		u.nameEd.SetText(u.st.Device.Name)
		return
	}
	if u.nameSaveBtn.Clicked(gtx) || submitted {
		u.editingName = false
		u.startRename(strings.TrimSpace(u.nameEd.Text()))
	}
}

// startRename is the rename job. It lives here rather than in
// actions.go because the field that starts it does too, and because it
// is the one job whose result the UI has to write back into
// config.json.
func (u *UI) startRename(name string) {
	vol, serial := u.st.Device.Volume, u.st.Device.Serial
	if vol == "" {
		u.st.Logf("rename: no iPod volume yet — plug the iPod in (disk mode: Select+Play)")
		return
	}
	u.start(JobRename, func(ctx context.Context, emit func(Event)) error {
		label, err := u.be.Rename(ctx, vol, name)
		if err != nil {
			return err
		}
		u.mu.Lock()
		u.newName = &renameResult{friendly: name, label: label, serial: serial}
		u.mu.Unlock()
		if name == "" {
			emit(Event{Kind: EventLog, Text: vol + ": the name was cleared"})
		} else {
			emit(Event{Kind: EventLog, Text: fmt.Sprintf(
				"%s: named %q — Windows shows the volume as %s", vol, name, label)})
		}
		return nil
	})
}

func (u *UI) dialogEvents(gtx C) {
	for {
		_, ok := u.confirmEd.Update(gtx)
		if !ok {
			break
		}
	}
	if u.dlgCancel.Clicked(gtx) {
		u.cancel()
		return
	}
	u.dlgBlock.Clicked(gtx) // a click on the plate is not a click outside it
	if u.dlg.solo() {
		// One button and nothing to cancel: Esc and a click outside close
		// it. Both reply CancelAnswer — closing is never a yes.
		if u.dlgScrim.Clicked(gtx) {
			u.cancel()
			return
		}
		for {
			ev, ok := gtx.Event(key.Filter{Name: key.NameEscape})
			if !ok {
				break
			}
			if ke, ok := ev.(key.Event); ok && ke.State == key.Press {
				u.cancel()
				return
			}
		}
	} else {
		u.dlgScrim.Clicked(gtx)
	}
	if u.planToggle.Clicked(gtx) {
		u.planOpen = !u.planOpen
	}
	if u.dlg.hold {
		// Drain the button's clicks: a click (or Enter on a focused
		// button) is not a hold and must not confirm anything.
		for {
			if _, ok := u.dlgHold.Update(gtx); !ok {
				break
			}
		}
		if u.holdFrozen > 0 {
			u.holdFrac = u.holdFrozen
			return
		}
		pressed := u.dlgHold.Pressed()
		f, done := u.holdTick(pressed, u.now())
		u.holdFrac = f
		if done {
			u.reply(u.dlg.holdAnswer)
			return
		}
		if pressed {
			gtx.Execute(op.InvalidateCmd{})
		}
		return
	}
	if u.dlgOK.Clicked(gtx) {
		u.ok()
	}
}

// CancelAnswer is the reply a dialog's Cancel sends — and the only one that
// means "no". Every job that asks reads "" as cancel.
const CancelAnswer = ""

// cancel is the Cancel button: always CancelAnswer, whatever the dialog.
// (It used to share one function with OK, which turned an empty answer
// into "yes" for a dialog with nothing to type — so Cancel on a dialog with
// nothing to type said yes.)
func (u *UI) cancel() { u.reply(CancelAnswer) }

// ok is the confirming button. A dialog with a typed field answers what was
// typed; a plain one answers "yes".
func (u *UI) ok() {
	if u.dlg == nil {
		return
	}
	if u.dlg.want != "" {
		u.reply(strings.TrimSpace(u.confirmEd.Text()))
		return
	}
	u.reply("yes")
}

// reply sends one answer and closes the dialog.
func (u *UI) reply(text string) {
	if u.dlg == nil {
		return
	}
	select {
	case u.dlg.reply <- text:
	default:
	}
	u.dlg = nil
	u.confirmEd.SetText("")
	u.holdStart, u.holdFrac, u.planOpen = time.Time{}, 0, false
}

// ask puts a modal question on screen and blocks the calling (job)
// goroutine until it is answered or the job is cancelled.
func (u *UI) ask(ctx context.Context, req *dialogRequest) (string, error) {
	req.reply = make(chan string, 1)
	if req.okLabel == "" {
		req.okLabel = "Confirm"
	}
	u.mu.Lock()
	u.askQueue = req
	u.mu.Unlock()
	u.invalidate()
	select {
	case s := <-req.reply:
		return s, nil
	case <-ctx.Done():
		return "", ctx.Err()
	}
}

// --- drawing -----------------------------------------------------------

func (u *UI) page(gtx C) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(u.topBar),
		layout.Rigid(u.progress),
		layout.Rigid(u.tabBar),
		layout.Flexed(1, func(gtx C) D {
			return layout.Inset{Left: unit.Dp(14), Right: unit.Dp(14),
				Top: unit.Dp(12), Bottom: unit.Dp(10)}.Layout(gtx, u.body)
		}),
		layout.Rigid(u.footer),
	)
}

// topBar is the window's one header: the brand, who this iPod is and what
// it is doing, and the actions — the ONE primary button, Eject beside it,
// and the "···" menu.
//
// The button's label is the decision (State.Primary), spelled as what it
// will do (State.PrimaryLabel): "Install Core" on a stock iPod, "Sync 7
// albums" when the library and the device differ, "Update to v0.2.2" when a
// newer release is known, Eject when there is nothing left. While a job that
// can be stopped runs, a Cancel takes the primary's place.
// NoticeFor is how long a State.Notice holds the header's line 2.
const NoticeFor = 5 * time.Second

func (u *UI) topBar(gtx C) D {
	if u.st.Notice != "" && u.now().Sub(u.st.NoticeAt) < NoticeFor {
		// Ask for the frame that takes the notice down again.
		gtx.Execute(op.InvalidateCmd{At: u.st.NoticeAt.Add(NoticeFor)})
	}
	dims := layout.Inset{Top: unit.Dp(10), Bottom: unit.Dp(10), Left: unit.Dp(14), Right: unit.Dp(14)}.
		Layout(gtx, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
				layout.Rigid(u.brand),
				layout.Rigid(layout.Spacer{Width: unit.Dp(20)}.Layout),
				layout.Flexed(1, u.deviceBlock),
				layout.Rigid(layout.Spacer{Width: unit.Dp(12)}.Layout),
				layout.Rigid(u.actions),
			)
		})
	u.topH = dims.Size.Y
	return dims
}

// brand is the lockup at the left of the top bar: the boot screen's mark and
// the word.
func (u *UI) brand(gtx C) D {
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(func(gtx C) D { return drawMark(gtx, unit.Dp(22), u.pal.Ink, u.pal.Surface) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D {
			l := material.Label(u.th, unit.Sp(16), "Core")
			l.Font.Weight = font.Bold
			l.Color = u.pal.Ink
			l.MaxLines = 1
			return l.Layout(gtx)
		}),
	)
}

// deviceBlock is the two lines in the middle of the bar: the iPod's name
// (and its model, when the name is one the user gave it), and one status
// line.
func (u *UI) deviceBlock(gtx C) D {
	name, model := u.headName()
	nameCol, weight := u.pal.Ink, font.Bold
	if !u.st.Device.Found {
		nameCol, weight = u.pal.Muted, font.SemiBold
	}
	status, statusCol := u.statusLine()
	// 1 dp above and below: the bar keeps its height, and the two lines
	// sit on the buttons' middle.
	return layout.Inset{Top: unit.Dp(1), Bottom: unit.Dp(1)}.Layout(gtx, func(gtx C) D {
		return u.deviceLines(gtx, name, model, nameCol, weight, status, statusCol)
	})
}

// deviceLines is the device block's two lines.
func (u *UI) deviceLines(gtx C, name, model string, nameCol color.NRGBA, weight font.Weight,
	status string, statusCol color.NRGBA) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return layout.Flex{Alignment: layout.Baseline}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					l := material.Label(u.th, unit.Sp(14), name)
					l.Font.Weight = weight
					l.Color = nameCol
					l.MaxLines = 1
					l.Truncator = " "
					return l.Layout(gtx)
				}),
				layout.Rigid(func(gtx C) D {
					if model == "" {
						return D{}
					}
					return layout.Spacer{Width: unit.Dp(8)}.Layout(gtx)
				}),
				layout.Flexed(1, func(gtx C) D {
					if model == "" {
						return D{}
					}
					l := material.Label(u.th, unit.Sp(12.5), model)
					l.Color = u.pal.Muted
					l.MaxLines = 1
					l.Truncator = " "
					return l.Layout(gtx)
				}),
			)
		}),
		layout.Rigid(layout.Spacer{Height: unit.Dp(2)}.Layout),
		// Not u.line: its 2 dp under the line would hang below the block
		// and lift both lines off the buttons' middle.
		layout.Rigid(func(gtx C) D {
			l := material.Label(u.th, unit.Sp(11.5), status)
			l.Color = statusCol
			l.MaxLines = 1
			l.Truncator = " "
			return l.Layout(gtx)
		}),
	)
}

// headName is line 1 of the device block: what to call this iPod, and the
// model beside it when the first half is a name the user chose. A name is
// remembered in config.json against the disk's serial; without one the
// model IS the name, and saying it twice would be noise.
func (u *UI) headName() (name, model string) {
	d := u.st.Device
	if !d.Found {
		return "No iPod yet", ""
	}
	m := strings.TrimSpace(d.Model)
	if m == "" {
		m = DefaultDeviceName
	}
	if u.st.Phase() == PhaseNotInstalled {
		return m, "Apple firmware"
	}
	if n := strings.TrimSpace(d.Name); n != "" {
		return n, m
	}
	return m, ""
}

// statusLine is line 2 of the device block, and its colour.
//
// A running job owns it ("sync: copying … · 42%"), except the update check,
// which is the "checking…" in the ordinary line — it runs by itself at
// launch, and a header that flickers to "check for updates: asking GitHub"
// every time the app opens is a header shouting about nothing. A job that
// failed is the one warning, in the accent; the check never fails here (its
// errors go to the log).
func (u *UI) statusLine() (string, color.NRGBA) {
	d, j := u.st.Device, u.st.Job
	if j != nil && j.Kind != JobCheck {
		switch {
		case j.Running():
			s := u.st.StatusLine()
			if j.Pct > 0 && j.Pct < 1 {
				s += fmt.Sprintf("  ·  %.0f%%", j.Pct*100)
			}
			return s, u.pal.MutedD
		case j.Failed:
			return u.st.StatusLine(), u.pal.Accent
		}
	}
	if u.st.Notice != "" && u.now().Sub(u.st.NoticeAt) < NoticeFor {
		return u.st.Notice, u.pal.MutedD
	}
	if u.st.FirmwareTooOld() {
		return TooOldText, u.pal.Accent
	}
	if u.st.Ejected && d.Found {
		return "Ejected — safe to unplug", u.pal.MutedD
	}
	switch u.st.Phase() {
	case PhaseLooking:
		if u.st.Detecting {
			return "looking every two seconds", u.pal.Muted
		}
		return "not connected", u.pal.Muted
	case PhaseNotInstalled:
		// The size, not the free space: nothing of ours is on it yet,
		// and "74.5 GB free of 74.5" reads as a riddle.
		n := d.Capacity
		if n <= 0 {
			n = d.Size
		}
		if n <= 0 {
			return "", u.pal.Muted
		}
		v, unit := humanBinary(n)
		return v + " " + unit, u.pal.Muted
	}
	var parts []string
	if tag := installedTag(d.Firmware); tag != "" {
		parts = append(parts, "Core "+tag)
	} else {
		parts = append(parts, "Core")
	}
	switch {
	case j.Running() && j.Kind == JobCheck:
		parts = append(parts, "checking…")
	case u.st.UpdateAvailable():
		parts = append(parts, u.st.Release.Tag+" available")
	case u.st.Release.Checked && u.st.Release.Err == "" && u.st.Release.Tag != "":
		parts = append(parts, "up to date")
	}
	if sp := u.spaceText(); sp != "" {
		parts = append(parts, sp)
	}
	return strings.Join(parts, "  ·  "), u.pal.Muted
}

// spaceText is "51.2 GB free of 74.5": the music volume's free space and
// size as its file system reports them, in the binary units Explorer calls
// GB. The total's unit is left off when it is the same as the free figure's.
// Before the volume has been read it is the disk's size, and with neither it
// is nothing.
func (u *UI) spaceText() string {
	d := u.st.Device
	if d.Capacity > 0 {
		fv, fu := humanBinary(d.Free)
		tv, tu := humanBinary(d.Capacity)
		if fu == tu {
			return fv + " " + fu + " free of " + tv
		}
		return fv + " " + fu + " free of " + tv + " " + tu
	}
	if d.Size > 0 {
		v, unit := humanBinary(d.Size)
		return v + " " + unit
	}
	return ""
}

// humanBinary is a byte count in 1024-based units with the names Windows
// gives them (GB, MB, KB), one decimal for GB.
func humanBinary(n int64) (string, string) {
	switch {
	case n >= 1<<30:
		return fmt.Sprintf("%.1f", float64(n)/(1<<30)), "GB"
	case n >= 1<<20:
		return fmt.Sprintf("%.0f", float64(n)/(1<<20)), "MB"
	default:
		return fmt.Sprintf("%.0f", float64(n)/(1<<10)), "KB"
	}
}

// actions is the right of the bar: primary (or Cancel), Eject, "···".
func (u *UI) actions(gtx C) D {
	act := u.st.Primary()
	busy := u.st.Busy()
	if u.st.Ejected && act == ActionEject {
		act = ActionNone
	}
	var children []layout.FlexChild
	add := func(w layout.Widget) {
		if len(children) > 0 {
			children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout))
		}
		children = append(children, layout.Rigid(w))
	}
	switch {
	case u.st.CanCancel():
		add(func(gtx C) D { return u.button(gtx, btn{&u.headCancelBtn, "Cancel", false, true}) })
	case act != ActionNone:
		add(func(gtx C) D {
			return u.button(gtx, btn{&u.primaryBtn, u.st.PrimaryLabel(), true, !busy})
		})
	}
	// Eject beside the primary, wherever the primary is something else
	// and there is a volume to let go of: "sync, then unplug" is one
	// session.
	if act != ActionEject && act != ActionNone && u.st.Device.Volume != "" {
		add(func(gtx C) D { return u.button(gtx, btn{&u.ejectBtn, "Eject", false, !busy}) })
	}
	add(func(gtx C) D { return u.button(gtx, btn{&u.menuBtn, "···", false, true}) })
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx, children...)
}

// Title is the window's title: the iPod's name while one is connected.
func (u *UI) Title() string {
	if !u.st.Device.Found {
		return "Core"
	}
	name, _ := u.headName()
	return name + " · Core"
}

// sourceName is the last element of the music folder, for a line that has
// room for a word and not for a path.
func sourceName(src string) string {
	src = strings.TrimRight(strings.TrimSpace(src), `\/`)
	if src == "" {
		return "your music folder"
	}
	if i := strings.LastIndexAny(src, `\/`); i >= 0 && i+1 < len(src) {
		return src[i+1:]
	}
	return src
}

// tabBar is the row under the top bar. It is only drawn on the main screen:
// the Looking and Install screens have one thing to say and tabs on them
// would offer a grid of an iPod that is not there.
func (u *UI) tabBar(gtx C) D {
	if u.st.Phase() != PhaseReady {
		return u.hairline(gtx)
	}
	tabs := []Tab{TabAlbums, TabLibrary, TabDetails}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return layout.Inset{Left: unit.Dp(14), Right: unit.Dp(14), Bottom: unit.Dp(2)}.
				Layout(gtx, func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					children := make([]layout.FlexChild, 0, len(tabs)*2+1)
					for i, t := range tabs {
						tab := t
						idx := i
						if i > 0 {
							children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(18)}.Layout))
						}
						children = append(children, layout.Rigid(func(gtx C) D {
							return u.tabItem(gtx, tab, idx)
						}))
					}
					children = append(children, layout.Flexed(1, func(gtx C) D {
						return layout.E.Layout(gtx, u.line(11, u.pal.Muted2, u.tabNote()))
					}))
					return layout.Flex{Alignment: layout.Middle}.Layout(gtx, children...)
				})
		}),
		layout.Rigid(u.hairline),
	)
}

// tabItem is one tab: the word, and a 2 px rule under the chosen one.
func (u *UI) tabItem(gtx C, t Tab, idx int) D {
	on := u.st.Tab == t
	col := u.pal.Muted
	if on {
		col = u.pal.Ink
	}
	label := t.String()
	if t == TabLibrary {
		if n := u.fixCount(); n > 0 {
			label += " · " + itoa(n)
		}
	}
	return u.tabClicks[idx].Layout(gtx, func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				return layout.Inset{Top: unit.Dp(6), Bottom: unit.Dp(4)}.Layout(gtx, func(gtx C) D {
					l := material.Label(u.th, unit.Sp(13), label)
					l.Color = col
					l.MaxLines = 1
					return l.Layout(gtx)
				})
			}),
			layout.Rigid(func(gtx C) D {
				h := gtx.Dp(2)
				c := u.pal.Surface
				if on {
					c = u.pal.Ink
				}
				sz := image.Pt(gtx.Constraints.Min.X, h)
				paint.FillShape(gtx.Ops, c, clip.Rect{Max: sz}.Op())
				return D{Size: sz}
			}),
		)
	})
}

// fixCount is the number on the Library tab: how many things the last scan
// found, tracks counted as tracks (see Category.Count).
func (u *UI) fixCount() int {
	n := 0
	for _, c := range Categories {
		n += c.Count(u.st.Report)
	}
	return n
}

// tabNote is the small right-hand line in the tab row: when the report was
// taken, or how many albums the grid holds.
func (u *UI) tabNote() string {
	switch u.st.Tab {
	case TabLibrary:
		if u.st.Report == nil {
			return "nothing scanned yet"
		}
		if u.st.Scanned.IsZero() {
			return "scanned"
		}
		return "scanned " + ago(time.Since(u.st.Scanned))
	case TabAlbums:
		if n := len(u.st.Albums); n > 0 {
			return fmt.Sprintf("%d albums · %d new · %d changed", n,
				countState(u.st.Albums, TileNew), countState(u.st.Albums, TileChanged))
		}
		return ""
	default:
		if u.st.CheckedAt.IsZero() {
			return "checked for updates: never"
		}
		return "checked for updates " + ago(time.Since(u.st.CheckedAt))
	}
}

// countState counts the tiles in one state.
func countState(albums []Album, st TileState) int {
	n := 0
	for _, a := range albums {
		if a.State == st {
			n++
		}
	}
	return n
}

// ago is a duration as the footer says it.
func ago(d time.Duration) string {
	switch {
	case d < time.Minute:
		return "just now"
	case d < time.Hour:
		return fmt.Sprintf("%d min ago", int(d.Minutes()))
	default:
		return fmt.Sprintf("%d h ago", int(d.Hours()))
	}
}

// hairline is the 1 px rule the bars are separated by.
func (u *UI) hairline(gtx C) D {
	sz := image.Pt(gtx.Constraints.Max.X, gtx.Dp(1))
	paint.FillShape(gtx.Ops, u.pal.Border, clip.Rect{Max: sz}.Op())
	return D{Size: sz}
}

// footer is the bottom strip: the music folder with the one link that
// changes it on the left, and the log strip on the right.
func (u *UI) footer(gtx C) D {
	dims := layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(u.hairline),
		layout.Rigid(func(gtx C) D {
			pad := unit.Dp(9)
			if u.st.NowPlaying != nil {
				// The bar's 28 dp buttons: less air, nearly the same footer.
				pad = 5
			}
			return layout.Inset{Top: pad, Bottom: pad, Left: unit.Dp(14), Right: unit.Dp(14)}.
				Layout(gtx, func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					half := gtx.Constraints.Max.X / 2
					return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
						layout.Rigid(func(gtx C) D {
							gtx.Constraints.Max.X = half - gtx.Dp(12)
							if np := u.st.NowPlaying; np != nil && np.TrackIndex < len(np.Album.TrackList) {
								// While a track is loaded the left half is
								// Now Playing; the music folder is on the
								// Library tab.
								gtx.Constraints.Min.X = gtx.Constraints.Max.X
								return u.nowPlayingBar(gtx)
							}
							if u.st.Phase() != PhaseReady {
								// No Library tab to go to.
								return u.line(11, u.pal.Muted, u.footerSource())(gtx)
							}
							return u.folderLink.Layout(gtx, func(gtx C) D {
								dims := u.line(11, u.pal.Muted, u.footerSource())(gtx)
								defer clip.Rect{Max: dims.Size}.Push(gtx.Ops).Pop()
								pointer.CursorPointer.Add(gtx.Ops)
								return dims
							})
						}),
						layout.Rigid(layout.Spacer{Width: unit.Dp(12)}.Layout),
						layout.Flexed(1, u.logStrip),
					)
				})
		}),
	)
	u.footH = dims.Size.Y
	return dims
}

// progress is the job's bar, in Ink like everything else that moves —
// except a failed job, which keeps its full bar in Accent: that is a
// warning, the one thing terracotta is for. An indeterminate
// job draws the full track in the muted colour rather than a fake
// fraction: a bar that claims 40 % when nothing knows the total is a bar
// that teaches people to distrust it.
func (u *UI) progress(gtx C) D {
	pct := float32(0)
	col := u.pal.Ink
	switch {
	case u.st.Job == nil:
		col = u.pal.Trk
	case u.st.Job.Failed:
		pct, col = 1, u.pal.Accent
	case u.st.Job.Pct < 0:
		pct, col = 1, u.pal.Muted2
	default:
		pct = u.st.Job.Pct
	}
	bar := material.ProgressBar(u.th, pct)
	bar.Color = col
	bar.TrackColor = u.pal.Trk
	bar.Height = unit.Dp(4)
	return bar.Layout(gtx)
}

// body is the phase switch: the whole middle of the window.
//
// The log pane below it does not move, because everything that happens
// in any phase says what it is doing there — a Looking screen that
// hides the reason the raw open was denied is a screen with one
// sentence and no way forward.
func (u *UI) body(gtx C) D {
	switch u.st.Phase() {
	case PhaseLooking:
		return u.lookingPane(gtx)
	case PhaseNotInstalled:
		return u.installPane(gtx)
	}
	switch u.st.Tab {
	case TabLibrary:
		return u.libraryPane(gtx)
	case TabDetails:
		return u.ipodPane(gtx)
	default:
		return u.albumsPane(gtx)
	}
}

// lookingPane is the screen with no iPod on it.
//
// It has no button at all. There is nothing for a button to do that the
// poll is not already doing twice a second, and a Refresh that a user
// presses because the screen implied it was needed teaches them the app
// does not notice things by itself.
func (u *UI) lookingPane(gtx C) D {
	d := u.st.Device
	// Direction centres within Min, so the pane claims the whole body.
	gtx.Constraints.Min = gtx.Constraints.Max
	return layout.Center.Layout(gtx, func(gtx C) D {
		if w := gtx.Dp(480); gtx.Constraints.Max.X > w {
			gtx.Constraints.Max.X = w
		}
		gtx.Constraints.Min = image.Point{}
		var rows []layout.FlexChild
		add := func(w layout.Widget) { rows = append(rows, layout.Rigid(w)) }
		gap := func(h unit.Dp) { rows = append(rows, layout.Rigid(layout.Spacer{Height: h}.Layout)) }
		centred := func(size unit.Sp, col color.NRGBA, txt string, weight font.Weight) layout.Widget {
			return func(gtx C) D {
				l := material.Label(u.th, size, txt)
				l.Color = col
				l.Font.Weight = weight
				l.Alignment = text.Middle
				l.MaxLines = 3
				return l.Layout(gtx)
			}
		}

		add(func(gtx C) D { return drawMark(gtx, unit.Dp(48), u.pal.Ink, u.pal.Surface) })
		gap(16)
		add(centred(18, u.pal.Ink, u.lookingHead(), font.Bold))
		gap(8)
		add(centred(12.5, u.pal.Muted, "Use a cable that carries data, then put the iPod in disk mode. "+
			"This window will notice on its own.", font.Normal))
		gap(16)
		add(func(gtx C) D {
			return u.step(gtx, "1.", []string{"Hold", "Select", "+", "Menu", "until it resets"})
		})
		gap(8)
		add(func(gtx C) D {
			return u.step(gtx, "2.", []string{"At the Apple logo, hold", "Select", "+", "Play"})
		})
		if !u.st.Detecting {
			gap(14)
			add(centred(11, u.pal.Muted2, "Auto-detect is off in this window (--no-detect).", font.Normal))
		}
		if d.Err != "" && d.Err != "the iPod was unplugged" {
			gap(14)
			add(centred(11, u.pal.Muted2, "last look:  "+d.Err, font.Normal))
		}
		if d.ElevationAdviceNeeded {
			gap(8)
			add(centred(11.5, u.pal.Accent, "Reading a raw disk needs Administrator: close this and "+
				"start it with Run as administrator.", font.Normal))
		}
		return layout.Flex{Axis: layout.Vertical, Alignment: layout.Middle}.Layout(gtx, rows...)
	})
}

// keyCaps names the words in a step that are the iPod's buttons.
var keyCaps = map[string]bool{"Select": true, "Menu": true, "Play": true}

// step is one line of instructions with the button names drawn as caps.
func (u *UI) step(gtx C, n string, words []string) D {
	children := []layout.FlexChild{layout.Rigid(u.text(13, u.pal.Muted, n))}
	for _, w := range words {
		w := w
		children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout))
		if keyCaps[w] {
			children = append(children, layout.Rigid(func(gtx C) D { return u.keyCap(gtx, w) }))
			continue
		}
		children = append(children, layout.Rigid(u.text(13, u.pal.Ink, w)))
	}
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx, children...)
}

// keyCap is a button name in a small bordered cap, the way a manual draws
// a key.
func (u *UI) keyCap(gtx C, label string) D {
	return widget.Border{Color: u.pal.Muted2, CornerRadius: unit.Dp(4), Width: unit.Dp(1)}.Layout(gtx,
		func(gtx C) D {
			return layout.Inset{Top: unit.Dp(2), Bottom: unit.Dp(2), Left: unit.Dp(7), Right: unit.Dp(7)}.
				Layout(gtx, func(gtx C) D {
					l := material.Label(u.th, unit.Sp(11.5), label)
					l.Font.Weight = font.SemiBold
					l.Color = u.pal.Ink
					return l.Layout(gtx)
				})
		})
}

func (u *UI) lookingHead() string {
	if u.st.Device.Err == "the iPod was unplugged" {
		return "The iPod was unplugged"
	}
	return "Plug in your iPod"
}

// installPane is the one screen a stock iPod gets: what was found, what
// will happen to it, and one button.
//
// The promise on it is the whole reason the screen exists rather than a
// dialog: Apple stopped distributing this firmware, the copy on the
// device is the only one, and the sentence saying where it is going is
// what makes pressing the button reasonable (library-manager-plan.md,
// decision 3).
func (u *UI) installPane(gtx C) D {
	d := u.st.Device
	busy := u.st.Busy()
	model := strings.TrimSpace(d.Model)
	if model == "" {
		model = DefaultDeviceName
	}
	return u.topPlate(gtx, func(gtx C) D {
		return layout.UniformInset(unit.Dp(18)).Layout(gtx, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(func(gtx C) D { return drawMark(gtx, unit.Dp(32), u.pal.Ink, u.pal.Plate) }),
				layout.Rigid(layout.Spacer{Height: unit.Dp(12)}.Layout),
				layout.Rigid(u.line(18, u.pal.Ink, "Core is not installed on this iPod")),
				layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
				layout.Rigid(u.line(13, u.pal.Ink, "found  "+model+"  ·  "+disk.HumanSize(d.Size))),
				layout.Rigid(layout.Spacer{Height: unit.Dp(12)}.Layout),
				layout.Rigid(u.wrap(12, u.pal.MutedD, u.appleBackupPromise())),
				layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
				layout.Rigid(u.wrap(12, u.pal.MutedD, "Then Core is written and read back. Your music is untouched.")),
				layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
				layout.Rigid(u.wrap(11, u.pal.Muted, "If anything goes wrong, hold Select+Menu, then Select+Play "+
					"at the Apple logo: that is disk mode, it is in ROM, and this window will find the iPod again.")),
				layout.Rigid(layout.Spacer{Height: unit.Dp(14)}.Layout),
				layout.Rigid(func(gtx C) D {
					bs := []btn{{&u.installBtn, "Install Core", true, !busy}}
					if f := u.st.InstallFile; f != "" {
						bs = append(bs,
							btn{&u.installFileBtn, "Install from " + shortName(fileBase(f), 28), false, !busy},
							btn{&u.installPickBtn, "Choose another file…", false, !busy && u.pickerOK})
					} else {
						bs = append(bs, btn{&u.installFileBtn, "Install from a file…", false, !busy && u.pickerOK})
					}
					return u.buttonRow(gtx, bs...)
				}),
			)
		})
	})
}

// shortName cuts a file name to n runes from the middle, keeping its end
// (the extension and the version usually sit there), so a long download
// name cannot push the Install row past a 720 dp window.
func shortName(s string, n int) string {
	r := []rune(s)
	if len(r) <= n || n < 5 {
		return s
	}
	head := (n - 1) / 2
	return string(r[:head]) + "…" + string(r[len(r)-(n-1-head):])
}

// fileBase is the last element of a path written on either OS, for a button
// that has room for a file name and not for where it lives.
func fileBase(p string) string {
	p = strings.TrimRight(p, `\/`)
	if i := strings.LastIndexAny(p, `\/`); i >= 0 {
		return p[i+1:]
	}
	return p
}

// topPlate is a plate that hugs its content at the top of the space it
// is given, rather than stretching to fill it. A card with three
// sentences in it and 300 px of empty plate underneath reads as a
// screen that failed to load the rest.
func (u *UI) topPlate(gtx C, w layout.Widget) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			gtx.Constraints.Min.Y = 0
			return u.plate(gtx, w)
		}),
	)
}

// appleBackupPromise is the Install screen's first sentence: what happens
// to Apple's firmware, in words. The file's name (it carries today's date)
// is in the log, where somebody looking for the file will look.
func (u *UI) appleBackupPromise() string {
	return "Apple's firmware on this iPod is saved to a file first — it is the only copy there is, " +
		"and it is never deleted."
}

// appleBackupName is the file that promise is about: the name the flasher
// will give Apple's firmware for this serial today. The log carries it (see
// startRefresh); the screen says it in words.
func appleBackupName(serial string) string {
	return flasher.AppleBackupFileName(serial, time.Now())
}

// --- the iPod's name and the firmware note ------------------------------

// Every line on a plate is one line: MaxLines 1 with a clean clip. A
// device path, a build id and a release note are all long enough to wrap
// at 340 px, and a pane that changes height because the string in it got
// longer is a pane that pushes its button row off the bottom of the
// window on the machine where it matters.

// namePreview is the sentence under the field while it is being typed
// in. A FAT label is eleven upper-case ASCII bytes, so most names come
// out shorter and louder than they went in, and finding that out in
// Explorer afterwards is how a rename reads as a bug.
func (u *UI) namePreview() string {
	typed := strings.TrimSpace(u.nameEd.Text())
	label := disk.LegalLabel(typed)
	switch {
	case typed == "":
		return "An empty name clears it: the drive shows with no name at all."
	case label == "":
		return "Nothing in that name fits a FAT label (A-Z, 0-9, space, !#$%&'()-@^_`{}~)."
	case label == typed:
		return "Windows will show it as " + label + "."
	default:
		return "Windows will show it as " + label + " — a FAT label is 11 upper-case ASCII bytes."
	}
}

// flashNote is the sentence that decides whether a user can flash at
// all. It names the CLI beside the app, or says it is missing and gives
// the command — an app that offered a Flash button it could not honour
// would be worse than no button.
func (u *UI) flashNote() string {
	name := "core"
	if runtime.GOOS == "windows" {
		name = "core.exe"
	}
	if u.st.CLIPath == "" {
		file := strings.TrimSpace(u.flashEd.Text())
		if file == "" {
			file = "<image>"
		}
		return name + " is not beside this app. Run:  " + name + " flash " + file
	}
	if u.st.Device.Elevated {
		return "This window has Administrator rights: the write runs here. The whole partition is backed up first and the write is read back."
	}
	if runtime.GOOS == "windows" {
		return "Runs " + name + " elevated (a UAC prompt appears). The whole partition is backed up first and the write is read back."
	}
	return "Needs root — the sudo line goes to the log. The whole partition is backed up first and the write is read back."
}

// --- the modal ----------------------------------------------------------

// --- small drawing helpers ---------------------------------------------

// line is one line of text: a value, held to one line so a card cannot
// change height because the string in it got longer. wrap is the same
// thing allowed to wrap, for the places that are a sentence.
//
// Truncator is a single space and not the default "…" on purpose.
// Gio v0.8 draws the truncator glyph at the START of the line rather
// than the end (checked here with a three-label headless render), so a
// line that overflows would read "…library 928 songs" — which looks
// like the beginning is missing when it is the end that is. Every
// string on a card is written to fit at the 720 px minimum; the
// truncation is the backstop for an unusually long serial or build id,
// and a clean clip is the honest way to show one.
func (u *UI) line(size unit.Sp, col color.NRGBA, txt string) layout.Widget {
	return func(gtx C) D {
		l := material.Label(u.th, size, txt)
		l.Color = col
		l.MaxLines = 1
		l.Truncator = " "
		return layout.Inset{Bottom: unit.Dp(2)}.Layout(gtx, l.Layout)
	}
}

func (u *UI) wrap(size unit.Sp, col color.NRGBA, txt string) layout.Widget {
	return func(gtx C) D {
		l := material.Label(u.th, size, txt)
		l.Color = col
		l.MaxLines = 3
		return layout.Inset{Bottom: unit.Dp(2)}.Layout(gtx, l.Layout)
	}
}

func (u *UI) text(size unit.Sp, col color.NRGBA, txt string) layout.Widget {
	return func(gtx C) D {
		l := material.Label(u.th, size, txt)
		l.Color = col
		return l.Layout(gtx)
	}
}

// plate is a card ground: the Plate fill with a hairline Border, which
// is the only elevation this design has. No shadows — the device has
// none and a drop shadow under a rectangle is the thing that makes a
// flat palette look like a mistake.
func (u *UI) plate(gtx C, w layout.Widget) D { return u.plateR(gtx, 8, w) }

// plateR is plate with a chosen corner radius.
func (u *UI) plateR(gtx C, radius unit.Dp, w layout.Widget) D {
	return widget.Border{Color: u.pal.Border, CornerRadius: radius, Width: unit.Dp(1)}.Layout(gtx,
		func(gtx C) D {
			return layout.Stack{}.Layout(gtx,
				layout.Expanded(func(gtx C) D {
					r := gtx.Dp(radius)
					defer clip.UniformRRect(image.Rectangle{Max: gtx.Constraints.Min}, r).Push(gtx.Ops).Pop()
					paint.ColorOp{Color: u.pal.Plate}.Add(gtx.Ops)
					paint.PaintOp{}.Add(gtx.Ops)
					return D{Size: gtx.Constraints.Min}
				}),
				layout.Stacked(func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					return w(gtx)
				}),
			)
		})
}

func (u *UI) card(gtx C, title string, body layout.Widget) D {
	return u.plate(gtx, func(gtx C) D {
		return layout.UniformInset(unit.Dp(10)).Layout(gtx, func(gtx C) D {
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					l := material.Label(u.th, unit.Sp(10), title)
					l.Color = u.pal.Muted2
					l.MaxLines = 1
					return l.Layout(gtx)
				}),
				layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
				layout.Rigid(body),
			)
		})
	})
}

// field is a bordered text input. material.Editor has no frame of its
// own, and an unframed input on a plate is indistinguishable from a
// label.
func (u *UI) field(gtx C, ed *widget.Editor, hint string) D {
	return widget.Border{Color: u.pal.Trk, CornerRadius: unit.Dp(5), Width: unit.Dp(1)}.Layout(gtx,
		func(gtx C) D {
			return layout.Stack{}.Layout(gtx,
				layout.Expanded(func(gtx C) D {
					r := gtx.Dp(5)
					defer clip.UniformRRect(image.Rectangle{Max: gtx.Constraints.Min}, r).Push(gtx.Ops).Pop()
					paint.ColorOp{Color: u.pal.Surface}.Add(gtx.Ops)
					paint.PaintOp{}.Add(gtx.Ops)
					return D{Size: gtx.Constraints.Min}
				}),
				layout.Stacked(func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					return layout.Inset{Top: unit.Dp(6), Bottom: unit.Dp(6), Left: unit.Dp(7), Right: unit.Dp(7)}.
						Layout(gtx, func(gtx C) D {
							e := material.Editor(u.th, ed, hint)
							e.TextSize = unit.Sp(12)
							e.Color = u.pal.Ink
							e.HintColor = u.pal.SelSub
							e.SelectionColor = u.pal.Trk
							return e.Layout(gtx)
						})
				}),
			)
		})
}

// btn is one button's whole description.
type btn struct {
	click   *widget.Clickable
	label   string
	primary bool
	enabled bool
}

func (u *UI) buttonRow(gtx C, bs ...btn) D {
	children := make([]layout.FlexChild, 0, len(bs)*2)
	for i := range bs {
		b := bs[i]
		if i > 0 {
			children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout))
		}
		children = append(children, layout.Rigid(func(gtx C) D { return u.button(gtx, b) }))
	}
	return layout.Flex{}.Layout(gtx, children...)
}

// button draws one. A disabled button keeps its shape and loses its
// contrast rather than disappearing: a row whose buttons come and go
// while a job runs is a row that moves under the pointer.
//
// The three looks are the device's. Primary is the iPod's selected row:
// an Ink bar with Surface text. Secondary is a Surface button with a
// hairline Border, Ink text. Disabled is the iPod's greyed row: the
// secondary shape with Muted text — a button that is off, not a slab of
// fill (PillOff vanishes into Onyx's plates). None of them is terracotta — that
// is kept for warnings.
func (u *UI) button(gtx C, b btn) D {
	bg, fg, edge := u.pal.Surface, u.pal.Ink, u.pal.Border
	switch {
	case !b.enabled:
		bg, fg, edge = u.pal.Surface, u.pal.Muted, u.pal.Border
		gtx = gtx.Disabled()
	case b.primary:
		bg, fg, edge = u.pal.Ink, u.pal.Surface, u.pal.Ink
	}
	return b.click.Layout(gtx, func(gtx C) D {
		return layout.Stack{Alignment: layout.Center}.Layout(gtx,
			layout.Expanded(func(gtx C) D {
				sz := gtx.Constraints.Min
				if b.enabled {
					defer clip.Rect{Max: sz}.Push(gtx.Ops).Pop()
					pointer.CursorPointer.Add(gtx.Ops)
				}
				r := gtx.Dp(5)
				paint.FillShape(gtx.Ops, edge,
					clip.UniformRRect(image.Rectangle{Max: sz}, r).Op(gtx.Ops))
				w := gtx.Dp(1)
				inner := image.Rectangle{Min: image.Pt(w, w), Max: sz.Sub(image.Pt(w, w))}
				paint.FillShape(gtx.Ops, bg, clip.UniformRRect(inner, r-w).Op(gtx.Ops))
				if b.click.Hovered() && b.enabled {
					// The hover is the fill moved a step toward the
					// text colour, the same for all three looks.
					paint.FillShape(gtx.Ops, withAlpha(fg, 0x18),
						clip.UniformRRect(inner, r-w).Op(gtx.Ops))
				}
				return D{Size: sz}
			}),
			layout.Stacked(func(gtx C) D {
				return layout.Inset{Top: unit.Dp(6), Bottom: unit.Dp(6), Left: unit.Dp(11), Right: unit.Dp(11)}.
					Layout(gtx, func(gtx C) D {
						l := material.Label(u.th, unit.Sp(12), b.label)
						l.Font.Weight = font.Bold
						l.Color = fg
						l.MaxLines = 1
						l.Alignment = text.Middle
						return l.Layout(gtx)
					})
			}),
		)
	})
}

// --- formatting ---------------------------------------------------------

func itoa(n int) string { return fmt.Sprintf("%d", n) }

func okWord(ok bool) string {
	if ok {
		return "OK"
	}
	return "BAD"
}

// comma groups a byte count the way the index's own size is quoted in
// the docs (237,584 B).
func comma(n int64) string {
	s := fmt.Sprintf("%d", n)
	if len(s) <= 3 {
		return s
	}
	var out []byte
	for i, c := range []byte(s) {
		if i > 0 && (len(s)-i)%3 == 0 {
			out = append(out, ',')
		}
		out = append(out, c)
	}
	return string(out)
}

// tail keeps the last n lines of a block, which is what a dialog has
// room for.
func tail(s string, n int) string {
	s = strings.TrimRight(strings.ReplaceAll(s, "\r\n", "\n"), "\n")
	lines := strings.Split(s, "\n")
	if len(lines) <= n {
		return s
	}
	return "…\n" + strings.Join(lines[len(lines)-n:], "\n")
}

// keepArt filters the candidate and decision maps down to the albums the
// new report still lists as coverless, returning nil maps when nothing
// survives so the tab reads exactly as it does before any lookup.
func keepArt(rep *librarian.Report, cands map[librarian.AlbumKey][]artfetch.Candidate,
	choices map[librarian.AlbumKey]librarian.Decision) (
	map[librarian.AlbumKey][]artfetch.Candidate, map[librarian.AlbumKey]librarian.Decision) {
	if rep == nil || (len(cands) == 0 && len(choices) == 0) {
		return nil, nil
	}
	live := map[librarian.AlbumKey]bool{}
	for _, a := range rep.MissingArt {
		live[a.Key] = true
	}
	var kc map[librarian.AlbumKey][]artfetch.Candidate
	for k, v := range cands {
		if live[k] {
			if kc == nil {
				kc = map[librarian.AlbumKey][]artfetch.Candidate{}
			}
			kc[k] = v
		}
	}
	var kd map[librarian.AlbumKey]librarian.Decision
	for k, v := range choices {
		if live[k] {
			if kd == nil {
				kd = map[librarian.AlbumKey]librarian.Decision{}
			}
			kd[k] = v
		}
	}
	return kc, kd
}
