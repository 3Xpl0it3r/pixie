package live

import (
	"errors"
	"fmt"
	"regexp"
	"strings"

	"github.com/gdamore/tcell"
	"github.com/rivo/tview"
	"github.com/sahilm/fuzzy"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/script"
)

type suggestion struct {
	name string
	desc string

	matchedIndexes []int
}

// Very rudimentary tokenizer. Not going to be fully robust, but it should be fine for our purposes.
func cmdTokenizer(input string) []string {
	re := regexp.MustCompile(`[\s=]+`)
	return re.Split(input, -1)
}

type autocompleter interface {
	GetSuggestions(string) []suggestion
}

func getNextCmdString(current, selection string) string {
	tokens := cmdTokenizer(current)
	if len(tokens) <= 1 {
		return selection
	}
	if len(tokens)%2 == 0 {
		// Even args means partial command and we can replace it.
		return strings.Join(tokens[:len(tokens)-1], " ") + " " + selection + " "
	}
	return current + " " + selection + " "
}

// autcompleteModal is the autocomplete modal.
type autocompleteModal struct {
	// Reference to the parent view.
	s *appState

	// The input box.
	ib *tview.InputField
	// The suggestion list.
	sl *tview.List
	// The description text box.
	dt *tview.TextView

	layout *tview.Flex

	// Current list of suggestions.
	suggestions    []suggestion
	scriptExecFunc func(*script.ExecutableScript)
}

func newAutocompleteModal(st *appState) *autocompleteModal {
	// The auto complete view consists of three widgets.:
	//  ------------------------------------------
	//  | Text box for search                    |
	//  |________________________________________|
	//  |  Suggestions       | Description       |
	//  |  List              |                   |
	//  |                    |                   |
	//  |                    |                   |
	//  |____________________|___________________|
	//
	// The description is updated when a specific suggestion is selection.
	// In the current state, entering tab whil on the text box will pick the
	// first suggestion.
	// Hitting down arrow will move to the suggestions list. Enter when
	// in the suggestions list will make it the active search.

	scriptInputBox := tview.NewInputField()
	scriptInputBox.
		SetBackgroundColor(tcell.ColorBlack)
	scriptInputBox.
		SetFieldBackgroundColor(tcell.ColorBlack).
		SetBorder(true)

	scriptListBox := tview.NewList()
	scriptListBox.
		ShowSecondaryText(false).
		SetBorder(true)

	scriptDescBox := tview.NewTextView()
	scriptDescBox.
		SetDynamicColors(true).
		SetBorder(true)

	// We need two layouts, one going | and the other going --.
	horiz := tview.NewFlex().
		SetDirection(tview.FlexColumn).
		AddItem(scriptListBox, 0, 5, false).
		AddItem(scriptDescBox, 0, 10, false)

	layout := tview.NewFlex().
		SetDirection(tview.FlexRow).
		AddItem(scriptInputBox, 3, 0, true).
		AddItem(horiz, 0, 1, false)

	return &autocompleteModal{
		s:      st,
		ib:     scriptInputBox,
		sl:     scriptListBox,
		dt:     scriptDescBox,
		layout: layout,
	}
}

// Show shows the modal.
func (m *autocompleteModal) validateScriptAndArgs(s string) (*script.ExecutableScript, error) {
	tokens := cmdTokenizer(s)
	if len(tokens) == 0 {
		return nil, errors.New("no script provided")
	}

	es, err := m.s.br.GetScript(tokens[0])
	if err != nil {
		return nil, err
	}

	if len(tokens) == 1 {
		return es, nil
	}
	fs := es.GetFlagSet()
	if fs == nil {
		return es, nil
	}
	err = fs.Parse(tokens[1:])
	if err != nil {
		return nil, err
	}
	if len(fs.Args()) != 0 {
		return nil, errors.New("extra unknown args provided")
	}
	es.UpdateFlags(fs)
	return es, nil
}

// Show shows the modal.
func (m *autocompleteModal) Show(app *tview.Application) tview.Primitive {
	// Start with suggestions based on empty input.
	m.suggestions = m.s.ac.GetSuggestions("")
	for idx, s := range m.suggestions {
		m.sl.InsertItem(idx, s.name, "", 0, nil)
	}
	if len(m.suggestions) != 0 {
		m.dt.SetText(m.suggestions[0].desc)
	}

	// Wire up the components.
	m.sl.SetChangedFunc(func(i int, scriptName string, s string, r rune) {
		if i < len(m.suggestions) {
			m.dt.SetText(m.suggestions[i].desc)
		}
	})

	m.sl.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		switch event.Key() {
		case tcell.KeyEnter:
			name := m.suggestions[m.sl.GetCurrentItem()].name
			m.ib.SetText(getNextCmdString(m.ib.GetText(), name))
			app.SetFocus(m.ib)
			return nil
		case tcell.KeyUp:
			// If you press up and on item zero move up to the input box.
			if m.sl.GetCurrentItem() == 0 {
				app.SetFocus(m.ib)
				return nil
			}
		}
		return event
	})

	m.ib.SetChangedFunc(func(currentText string) {
		commandAndArgs := stripColors(currentText)
		m.suggestions = m.s.ac.GetSuggestions(commandAndArgs)
		m.sl.Clear()
		for i, s := range m.suggestions {
			sb := strings.Builder{}
			for i := 0; i < len(s.name); i++ {
				if contains(i, s.matchedIndexes) {
					sb.WriteString(fmt.Sprintf("[green]%s[white]", string(s.name[i])))
				} else {
					sb.WriteByte(s.name[i])
				}
			}

			m.sl.InsertItem(i, sb.String(), s.name, 0, nil)
		}
		_, err := m.validateScriptAndArgs(currentText)
		if err == nil {
			// Valid script.
			m.ib.SetBorderColor(tcell.ColorGreen)
		} else {
			m.ib.SetBorderColor(tcell.ColorYellow)
		}

	})

	m.ib.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		switch event.Key() {
		case tcell.KeyDown:
			app.SetFocus(m.sl)
			return nil
		case tcell.KeyEnter:
			scriptAndArgs := stripColors(m.ib.GetText())
			execScript, err := m.validateScriptAndArgs(scriptAndArgs)
			if err != nil {
				return event
			}
			if m.scriptExecFunc != nil {
				m.scriptExecFunc(execScript)
			}
		case tcell.KeyTAB:
			if len(m.suggestions) == 1 {
				value := m.suggestions[0].name
				m.ib.SetText(getNextCmdString(m.ib.GetText(), stripColors(value)))
			} else {
				app.SetFocus(m.sl)
			}
		}

		return event
	})

	app.SetFocus(m.ib)
	return m.layout
}

func (m *autocompleteModal) setScriptExecFunc(f func(s *script.ExecutableScript)) {
	m.scriptExecFunc = f
}

// Close is called when the modal is closed. Nothing to do for autocomplete.
func (m *autocompleteModal) Close(app *tview.Application) {}

type fuzzyAutocompleter struct {
	br *script.BundleManager
	// We cache the script names to make it easier to do searches.
	scriptNames []string

	shouldAppend bool
}

func newFuzzyAutoCompleter(br *script.BundleManager) *fuzzyAutocompleter {
	scripts := br.GetScripts()
	scriptNames := make([]string, len(scripts))
	for idx, s := range scripts {
		scriptNames[idx] = s.ScriptName
	}

	return &fuzzyAutocompleter{
		br:          br,
		scriptNames: scriptNames,
	}
}

func (f *fuzzyAutocompleter) isValidScript(scriptName string) bool {
	_, err := f.br.GetScript(scriptName)
	return err == nil
}

// GetSuggestions returns a list of suggestions.
func (f *fuzzyAutocompleter) GetSuggestions(input string) []suggestion {
	f.shouldAppend = false
	inputArr := cmdTokenizer(input)
	// If the input is empty return all possible values.
	if len(input) == 0 || len(inputArr) < 1 {
		suggestions := make([]suggestion, len(f.scriptNames))
		for i, sn := range f.scriptNames {
			suggestions[i] = suggestion{
				name:           sn,
				desc:           f.br.MustGetScript(sn).LongDoc,
				matchedIndexes: nil,
			}
		}
		return suggestions
	}

	if len(inputArr) == 1 || !f.isValidScript(inputArr[0]) {
		matches := fuzzy.Find(inputArr[0], f.scriptNames)
		suggestions := make([]suggestion, len(matches))
		for i, m := range matches {
			suggestions[i] = suggestion{
				name:           m.Str,
				desc:           f.br.MustGetScript(m.Str).LongDoc,
				matchedIndexes: m.MatchedIndexes,
			}
		}
		return suggestions
	}
	// This is a placeholder until we get proper autocomplete in.
	// Do argument completion ...
	es, err := f.br.GetScript(inputArr[0])
	if err != nil {
		return nil
	}
	if es.Vis == nil || es.Vis.Variables == nil {
		return nil
	}

	allSuggestionsMap := make(map[string]suggestion, 0)
	argNames := make([]string, 0)
	for _, arg := range es.Vis.Variables {
		name := fmt.Sprintf("--%s", arg.Name)
		argNames = append(argNames, name)
		allSuggestionsMap[name] = suggestion{
			name:           name,
			desc:           fmt.Sprintf("Description : %s, \n\nDefault :%s", arg.Description, arg.DefaultValue),
			matchedIndexes: nil,
		}
	}

	// Only show suggestions if we have an odd number of values (script + complete args).
	if len(inputArr)%2 == 1 {
		return nil
	}
	// If empty return all the values for the arguments.
	lastArg := inputArr[len(inputArr)-1]
	if lastArg == "" {
		suggestions := make([]suggestion, 0)
		for _, v := range allSuggestionsMap {
			suggestions = append(suggestions, v)
		}
		return suggestions
	}

	// Else do the suggestion match:
	matches := fuzzy.Find(lastArg, argNames)
	suggestions := make([]suggestion, len(matches))
	for i, m := range matches {
		suggestions[i] = suggestion{
			name:           m.Str,
			desc:           allSuggestionsMap[m.Str].desc,
			matchedIndexes: m.MatchedIndexes,
		}
	}
	return suggestions
}
