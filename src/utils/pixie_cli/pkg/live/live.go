package live

import (
	"context"
	"errors"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode"

	"github.com/alecthomas/chroma/quick"
	"github.com/gdamore/tcell"
	"github.com/rivo/tview"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/components"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/script"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/vizier"
)

const (
	debugShowBorders = false
	maxCellSize      = 50
	logoColor        = "#3FE7E7"
	textColor        = "#ffffff"
	accentColor      = "#008B8B"
	bgColor          = "#000000"
)

type modalType int

const (
	modalTypeUnknown modalType = iota
	modalTypeHelp
	modalTypeAutocomplete
)

type sortType int

const (
	stUnsorted = iota
	stAsc
	stDesc
)

// appState is the global state that is used by the live view.
type appState struct {
	br *script.BundleManager
	ac autocompleter

	vizier *vizier.Connector
	// The last script that was executed. If nil, nothing was executed.
	execScript *script.ExecutableScript
	// The view of all the tables in the current execution.
	tables []components.TableView
	// Sort state is tracked on a per table basis for each column. It is cleared when a new
	// script is executed.
	sortState [][]sortType
	// ----- View Specific State ------
	// The currently selected table. Will reset to zero when new tables are inserted.
	selectedTable int

	scriptViewOpen bool

	// State for search input box.
	searchBoxEnabled bool
	searchEnterHit   bool
	searchString     string
}

// View is the top level of the Live View.
type View struct {
	app           *tview.Application
	pages         *tview.Pages
	tableSelector *tview.TextView
	infoView      *tview.TextView
	tvTable       *tview.Table
	logoBox       *tview.TextView
	bottomBar     *tview.Flex
	searchBox     *tview.InputField
	modal         Modal
	s             *appState
}

// Modal is the interface for a pop-up view.
type Modal interface {
	Show(a *tview.Application) tview.Primitive
	Close(a *tview.Application)
}

// New creates a new live view.
func New(br *script.BundleManager, vizier *vizier.Connector, execScript *script.ExecutableScript) (*View, error) {
	// App is the top level view. The layout is approximately as follows:
	//  ------------------------------------------
	//  | View Information ...                   |
	//  |________________________________________|
	//  | The actual tables                      |
	//  |                                        |
	//  |                                        |
	//  |                                        |
	//  |________________________________________|
	//  | Table Selector                | Logo   |
	//  ------------------------------------------

	// Top of page.
	infoView := tview.NewTextView()
	infoView.
		SetScrollable(false).
		SetDynamicColors(true).
		SetBorder(debugShowBorders)
	infoView.SetBorderPadding(1, 0, 0, 0)
	topBar := tview.NewFlex().
		SetDirection(tview.FlexRow).
		AddItem(infoView, 3, 0, true)

	// Middle of page.
	pages := tview.NewPages()
	pages.SetBorder(debugShowBorders)

	// Bottom of Page.
	logoBox := tview.NewTextView().
		SetScrollable(false).
		SetDynamicColors(true)

	// Print out the logo.
	fmt.Fprintf(logoBox, "\n  [%s]PIXIE[%s]", logoColor, textColor)

	tableSelector := tview.NewTextView()
	bottomBar := tview.NewFlex().
		SetDirection(tview.FlexColumn).
		AddItem(tableSelector, 0, 1, false).
		AddItem(logoBox, 8, 1, false)
	bottomBar.SetBorderPadding(1, 0, 0, 0)

	layout := tview.NewFlex().
		SetDirection(tview.FlexRow).
		AddItem(topBar, 3, 0, false).
		AddItem(pages, 0, 1, true).
		AddItem(bottomBar, 2, 0, false)

	searchBox := tview.NewInputField()
	searchBox.SetBackgroundColor(tcell.ColorBlack)
	searchBox.SetFieldBackgroundColor(tcell.ColorBlack)

	// Application setup.
	app := tview.NewApplication()
	app.SetRoot(layout, true).
		EnableMouse(true)

	v := &View{
		app:           app,
		pages:         pages,
		tableSelector: tableSelector,
		infoView:      infoView,
		logoBox:       logoBox,
		searchBox:     searchBox,
		bottomBar:     bottomBar,
		s: &appState{
			br:     br,
			vizier: vizier,
			ac:     newFuzzyAutoCompleter(br),
		},
	}

	// Wire up components.
	tableSelector.
		SetDynamicColors(true).
		SetRegions(true).
		SetWrap(false)

	// When table selector is highlighted (ie. mouse click or number). We use the region
	// to select the appropriate table.
	tableSelector.SetHighlightedFunc(func(added, removed, remaining []string) {
		if len(added) > 0 {
			if tableNum, err := strconv.Atoi(added[0]); err == nil {
				v.selectTable(tableNum)
			}
		}
	})

	searchBox.SetChangedFunc(v.search)
	searchBox.SetInputCapture(v.searchInputCapture)
	// If a default script was passed in execute it.
	v.runScript(execScript)

	// Wire up the main keyboard handler.
	app.SetInputCapture(v.keyHandler)
	return v, nil
}

// Run runs the view.
func (v *View) Run() error {
	return v.app.Run()
}

// Stop stops the view and kills the app.
func (v *View) Stop() {
	v.app.Stop()
}

// runScript is the internal method to run an executable script and update relevant appState.
func (v *View) runScript(execScript *script.ExecutableScript) {
	v.clearErrorIfAny()
	if execScript == nil {
		v.execCompleteWithError(errors.New("No script provided"))
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	resp, err := vizier.RunScript(ctx, v.s.vizier, execScript)
	if err != nil {
		return
	}
	tw := vizier.NewVizierStreamOutputAdapter(ctx, resp, vizier.FormatInMemory)
	err = tw.Finish()
	if err != nil {
		v.execCompleteWithError(err)
		return
	}

	v.s.tables, err = tw.Views()
	if err != nil {
		v.execCompleteWithError(err)
		return
	}
	// Reset sort state.
	v.s.sortState = make([][]sortType, len(v.s.tables))
	for i, t := range v.s.tables {
		// Default value is unsorted.
		v.s.sortState[i] = make([]sortType, len(t.Header()))
	}
	// The view can update with nil data if there is an error.
	v.s.execScript = execScript
	v.s.selectedTable = 0

	v.execCompleteViewUpdate()
	return
}

func (v *View) clearErrorIfAny() {
	// Clear error pages if any.
	if v.pages.HasPage("error") {
		v.pages.RemovePage("error")
	}
}
func (v *View) execCompleteWithError(err error) {
	v.searchClear()
	v.closeModal()

	m := vizier.FormatErrorMessage(err)
	if v.s.execScript == nil {
		m += "\nType '?' for help or ctrl-k to get started."
	}
	tv := tview.NewTextView()
	tv.SetDynamicColors(true)
	tv.SetText(tview.TranslateANSI(m))

	v.s.selectedTable = 0
	v.tvTable = nil
	v.pages.AddAndSwitchToPage("error", tv, true)
	v.app.SetFocus(tv)
}

func (v *View) execCompleteViewUpdate() {
	v.closeModal()
	v.searchClear()

	v.updateScriptInfoView()
	v.updateTableNav()
	v.renderCurrentTable()
}

func (v *View) updateScriptInfoView() {
	v.infoView.Clear()

	fmt.Fprintf(v.infoView, "%s %s", withAccent("Script:"),
		v.s.execScript.ScriptName)
	args := v.s.execScript.ComputedArgs()
	if len(args) > 0 {
		for _, arg := range args {
			fmt.Fprintf(v.infoView, " --%s=%s ", withAccent(arg.Name), arg.Value)
		}
	}

	fmt.Fprintf(v.infoView, "\n")
	if lvl := v.s.execScript.LiveViewLink(); lvl != "" {
		fmt.Fprintf(v.infoView, "%s %s", withAccent("Live View:"), lvl)
	}
}

func (v *View) renderCurrentTable() {
	// We remove all the old pages and create new pages for tables.
	if v.pages.HasPage("table") {
		v.pages.RemovePage("table")
	}

	if len(v.s.tables) < v.s.selectedTable {
		return
	}
	table := v.s.tables[v.s.selectedTable]
	v.tvTable = v.createTviewTable(table, v.s.sortState[v.s.selectedTable])
	v.pages.AddAndSwitchToPage("table", v.tvTable, true)
	v.app.SetFocus(v.pages)
}

func (v *View) updateTableNav() {
	v.tableSelector.Clear()
	for idx, t := range v.s.tables {
		fmt.Fprintf(v.tableSelector, `%d ["%d"]%s[""]  `, idx+1, idx, withAccent(t.Name()))
	}
	v.showTableNav()
}

func (v *View) selectNextTable() {
	v.selectTableAndHighlight(v.s.selectedTable + 1)
}

func (v *View) selectPrevTable() {
	v.selectTableAndHighlight(v.s.selectedTable - 1)
}

func (v *View) createTviewTable(t components.TableView, sortState []sortType) *tview.Table {
	table := tview.NewTable().
		SetBorders(true).
		SetSelectable(true, true).
		SetFixed(1, 0)

	for idx, val := range t.Header() {
		// Render the header.
		tableCell := tview.NewTableCell(withAccent(val) + sortIcon(sortState[idx])).
			SetAlign(tview.AlignCenter).
			SetSelectable(false).
			SetExpansion(2)
		table.SetCell(0, idx, tableCell)
	}

	data := t.Data()
	// Sort columns from left to right.
	sorting := false
	for _, order := range sortState {
		if order != stUnsorted {
			sorting = true
		}
	}
	if sorting {
		sort.SliceStable(data, func(i, j int) bool {
			// TODO(zasgar): Clean this up when we have properly type columns.
			for idx, order := range sortState {
				if order == stUnsorted {
					continue
				}
				return colCompare(data[i][idx], data[j][idx], order)
			}
			return false
		})
	}

	// Sort the data according to the
	for rowIdx, row := range data {
		for colIdx, val := range stringifyRow(row) {
			if len(val) > maxCellSize {
				val = val[:maxCellSize-1] + "\u2026"
			}

			tableCell := tview.NewTableCell(tview.TranslateANSI(val)).
				SetTextColor(tcell.ColorWhite).
				SetAlign(tview.AlignLeft).
				SetSelectable(true).
				SetExpansion(2)
			table.SetCell(rowIdx+1, colIdx, tableCell)
		}
	}

	handleLargeBlobView := func(row, column int) {
		v.closeModal()

		if row < 1 || column < 0 {
			return
		}

		// Try to parse large blob as a string, we only know how to render large strings
		// so bail if we can't convert to string or if it's not that big.
		d := t.Data()[row-1][column]
		s, ok := d.(string)
		if !ok || len(s) < maxCellSize {
			return
		}

		renderString := tryJSONHighlight(s)
		v.showDataModal(tview.TranslateANSI(renderString))
	}

	// Since selection and mouse clicks happen in two different events, we need to track the selection
	// rows/cols in variables so that we can show the right popup.
	selectedRow := 0
	selectedCol := 0
	table.SetMouseCapture(func(action tview.MouseAction, event *tcell.EventMouse) (tview.MouseAction, *tcell.EventMouse) {
		if action == tview.MouseLeftDoubleClick {
			handleLargeBlobView(selectedRow, selectedCol)
			// For some reason the double click event does not trigger a redraw.
			v.app.ForceDraw()
			return action, event
		}
		return action, event
	})

	table.SetSelectionChangedFunc(func(row, column int) {
		//fmt.Printf("%+v  %+v\n", row, column)
		// Switch the sort state.
		if row == 0 {
			cs := v.s.sortState[v.s.selectedTable][column]
			v.s.sortState[v.s.selectedTable][column] = nextSort(cs)
			v.renderCurrentTable()
		}
		// Store the selection so we can pop open the blob view on double click.
		selectedRow = row
		selectedCol = column
		// This function is triggered when mouse is used after modal is open, in which case we can switch the blob.
		if v.modal != nil {
			handleLargeBlobView(row, column)
		}
	})

	table.SetSelectedFunc(handleLargeBlobView)

	return table
}

func (v *View) showScriptView() {
	v.s.scriptViewOpen = true
	tv := tview.NewTextView()
	tv.SetDynamicColors(true)
	v.pages.AddAndSwitchToPage("script", tv, true)
	if v.s.execScript != nil {
		highlighted := strings.Builder{}
		quick.Highlight(&highlighted, v.s.execScript.ScriptString, "python",
			"terminal16m", "monokai")
		fmt.Fprintf(tv, "%s :\n\n", withAccent("Script"))
		fmt.Fprint(tv, tview.TranslateANSI(highlighted.String()))
	} else {
		fmt.Fprintf(tv, "[red]Script Not Found[white]")
	}

	v.app.SetFocus(tv)
}

func (v *View) closeScriptView() {
	if !v.s.scriptViewOpen {
		return
	}
	v.pages.RemovePage("script")
	v.s.scriptViewOpen = false
	v.selectTableAndHighlight(v.s.selectedTable)
}

func (v *View) showDataModal(s string) {
	v.closeModal()
	d := newDetailsModal(s)
	m := d.Show(v.app)
	v.pages.AddPage("modal", createModal(m, 60, 30), true, true)
	v.modal = d
}

func (v *View) showAutcompleteModal() {
	v.closeModal()
	ac := newAutocompleteModal(v.s)
	ac.setScriptExecFunc(func(s *script.ExecutableScript) {
		v.runScript(s)
	})
	v.modal = ac
	v.pages.AddPage("modal", createModal(v.modal.Show(v.app),
		65, 30), true, true)
}

func (v *View) showHelpModal() {
	v.closeModal()
	hm := &helpModal{}
	v.modal = hm
	v.pages.AddPage("modal", createModal(v.modal.Show(v.app),
		65, 30), true, true)
}

// closes modal if open, noop if not.
func (v *View) closeModal() {
	if v.modal == nil {
		return
	}
	v.pages.RemovePage("modal")
	v.modal = nil
	if v.s.searchBoxEnabled {
		// This will refocus the search box.
		v.showSearchBox()
	} else {
		// This will cause a refocus to occur on the table.
		v.selectTableAndHighlight(v.s.selectedTable)
	}
}

// selectTableAndHighlight selects and highligts the table. Don't call this from within the highlight func
// or you will get an infinite loop.
func (v *View) selectTableAndHighlight(tableNum int) {
	tableNum = v.selectTable(tableNum)
	v.tableSelector.Highlight(strconv.Itoa(tableNum)).ScrollToHighlight()
}

func (v *View) showTableNav() {
	v.s.searchBoxEnabled = false
	// Clear the text box.
	v.searchClear()
	v.bottomBar.
		Clear().
		AddItem(v.tableSelector, 0, 1, false).
		AddItem(v.logoBox, 8, 1, false)

	// Switch focus back to the active table.
	v.selectTableAndHighlight(v.s.selectedTable)
}

func (v *View) showSearchBox() {
	v.s.searchBoxEnabled = true
	v.bottomBar.
		Clear().
		AddItem(v.searchBox, 0, 1, false).
		AddItem(v.logoBox, 8, 1, false)
	v.app.SetFocus(v.searchBox)
}

// selectTable selects the numbered table. Out of bounds wrap in both directions.
func (v *View) selectTable(tableNum int) int {
	if v.s.scriptViewOpen {
		v.closeScriptView()
	}
	if len(v.s.tables) == 0 {
		return 0
	}
	tableNum = tableNum % len(v.s.tables)

	// We only need to render if it's a different table.
	if v.s.selectedTable != tableNum {
		v.s.selectedTable = tableNum
		v.renderCurrentTable()
	}
	v.app.SetFocus(v.pages)

	return tableNum
}

func (v *View) activeModalType() modalType {
	if v.modal == nil {
		return modalTypeUnknown
	}
	switch v.modal.(type) {
	case *helpModal:
		return modalTypeHelp
	case *autocompleteModal:
		return modalTypeAutocomplete
	default:
		return modalTypeUnknown
	}
}

func (v *View) searchClear() {
	v.s.searchEnterHit = false
	v.s.searchString = ""
	v.searchBox.SetText("")
}

func (v *View) search(s string) {
	v.s.searchString = s
	v.searchNext(false, false)
}

func (v *View) searchNext(searchBackwards bool, advance bool) {
	s := v.s.searchString
	if s == "" {
		return
	}

	// Very unoptimized search function...
	if v.tvTable == nil {
		return
	}
	t := v.tvTable
	rc := t.GetRowCount()
	cc := t.GetColumnCount()

	searchFunc := func(t string) bool {
		return strings.Contains(t, s)
	}

	// If possible try to make it a regexp.
	re, err := regexp.Compile(s)
	if err == nil {
		searchFunc = func(t string) bool {
			return re.Match([]byte(t))
		}
	}
	wrappedCount := 0
	for wrappedCount < 2 {
		r, c := t.GetSelection()
		if advance {
			c++
		}
		rowCond := func() bool {
			return r < rc
		}
		colCond := func() bool {
			return c < cc
		}

		if searchBackwards {
			if advance {
				c -= 2 // For the increment above, and back one more col.
			}
			rowCond = func() bool {
				return r >= 0
			}
			colCond = func() bool {
				return c >= 0
			}
		}

		for rowCond() {
			for colCond() {
				if searchFunc(stripColors(t.GetCell(r, c).Text)) {
					t.Select(r, c)
					return
				}
				if searchBackwards {
					c--
				} else {
					c++
				}
			}
			if searchBackwards {
				c = cc
				r--
			} else {
				c = 0
				r++
			}
		}

		// Roll over.
		if searchBackwards {
			t.Select(rc, cc)
		} else {
			t.Select(0, 0)
		}
		wrappedCount++
	}
}

func (v *View) searchInputCapture(event *tcell.EventKey) *tcell.EventKey {
	switch event.Key() {
	case tcell.KeyEnter:
		v.s.searchEnterHit = true
		return nil
	case tcell.KeyBackspace2:
		fallthrough
	case tcell.KeyBackspace:
		fallthrough
	case tcell.KeyDelete:
		v.s.searchEnterHit = false
		return event
	case tcell.KeyCtrlR:
		v.searchNext(true, true)
	case tcell.KeyCtrlS:
		v.searchNext(false, true)
	case tcell.KeyRune:
		if v.s.searchEnterHit {
			s := string(event.Rune())
			if s == "n" {
				v.searchNext(false, true)
			}
			if s == "p" {
				v.searchNext(true, true)
			}
			return nil
		}
	case tcell.KeyEscape:
		v.searchClear()
		v.showTableNav()
		return nil
	}
	return event
}

func (v *View) keyHandler(event *tcell.EventKey) *tcell.EventKey {
	// If the modal is open capture the event and let escape or the original
	// shortcut close it.
	if v.modal != nil {
		switch event.Key() {
		case tcell.KeyEscape:
			v.closeModal()
			return nil
		case tcell.KeyRune:
			switch string(event.Rune()) {
			case "?":
				if v.activeModalType() == modalTypeHelp {
					v.closeModal()
					return nil
				}
			}
		case tcell.KeyCtrlK:
			if v.activeModalType() == modalTypeAutocomplete {
				v.closeModal()
				return nil
			}
		}
		return event
	}

	if v.s.searchBoxEnabled {
		if event.Key() == tcell.KeyCtrlK {
			v.showAutcompleteModal()
		}
		return event
	}

	switch event.Key() {
	case tcell.KeyTAB:
		// Default for tab is to quit so stop that.
		return nil
	case tcell.KeyCtrlN:
		v.selectNextTable()
	case tcell.KeyCtrlP:
		v.selectPrevTable()
	case tcell.KeyRune:
		// Switch to a specific view. This will be a no-op if no tables are loaded.
		r := event.Rune()
		if unicode.IsDigit(r) {
			v.selectTableAndHighlight(int(r-'0') - 1)
		}

		if string(r) == "?" {
			v.showHelpModal()
			return nil
		}
		if string(r) == "/" {
			if v.s.searchBoxEnabled {
				v.showTableNav()
				return nil
			}
			v.showSearchBox()
			return nil
		}
	case tcell.KeyCtrlS:
		v.showSearchBox()
		return nil
	case tcell.KeyCtrlV:
		if v.s.scriptViewOpen {
			v.closeScriptView()
			return nil
		}
		v.showScriptView()
		return nil
	case tcell.KeyCtrlK:
		v.showAutcompleteModal()
		return nil
	}

	// Ctrl-c, etc. can happen based on default handlers.
	return event
}

// TODO(zasgar): Share this functions with regular table renderer.
type stringer interface {
	String() string
}

func stringifyRow(row []interface{}) []string {
	s := make([]string, len(row))

	for i, val := range row {
		switch u := val.(type) {
		case time.Time:
			s[i] = u.Format(time.RFC3339)
		case stringer:
			s[i] = u.String()
		case float64:
			s[i] = fmt.Sprintf("%0.2f", u)
		default:
			s[i] = fmt.Sprintf("%+v", u)
		}
	}
	return s
}

func sortIcon(s sortType) string {
	switch s {
	case stUnsorted:
		return " \u2195"
	case stAsc:
		return " \u2191"
	case stDesc:
		return " \u2193"
	}
	return ""
}

func nextSort(s sortType) sortType {
	switch s {
	case stUnsorted:
		return stAsc
	case stAsc:
		return stDesc
	case stDesc:
		fallthrough
	default:
		return stUnsorted
	}
}

func colCompare(v1 interface{}, v2 interface{}, s sortType) bool {
	switch v1c := v1.(type) {
	case time.Time:
		v2c, ok := v2.(time.Time)
		if !ok {
			break
		}
		if s == stAsc {
			return v1c.Sub(v2c) > 0
		}
		return v1c.Sub(v2c) < 0
	case float64:
		v2c, ok := v2.(float64)
		if !ok {
			break
		}
		if s == stAsc {
			return v1c < v2c
		}
		return v2c < v1c
	case int64:
		v2c, ok := v2.(int64)
		if !ok {
			break
		}
		if s == stAsc {
			return v1c < v2c
		}
		return v2c < v1c
	}

	// Sort as strings, since we can't find type.
	v1c := fmt.Sprintf("%+v", v1)
	v2c := fmt.Sprintf("%+v", v2)
	if s == stAsc {
		return v1c < v2c
	}
	return v2c < v1c
}
