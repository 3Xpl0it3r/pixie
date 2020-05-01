package autocomplete

import (
	"errors"
	"fmt"
	"strings"

	uuid "github.com/satori/go.uuid"

	"pixielabs.ai/pixielabs/src/cloud/autocomplete/ebnf"
	"pixielabs.ai/pixielabs/src/cloud/cloudapipb"
)

// CursorMarker is the string that is used to denote the position of the cursor in the formatted output string.
var CursorMarker = "$0"

// Suggester is responsible for providing suggestions.
type Suggester interface {
	// GetSuggestions does a fuzzy match on the given input.
	GetSuggestions(reqs []*SuggestionRequest) ([]*SuggestionResult, error)
}

// Suggestion is a suggestion for a token.
type Suggestion struct {
	Name     string
	Desc     string
	Score    float64 // A score representing how closely the suggestion matches the given input.
	Kind     cloudapipb.AutocompleteEntityKind
	ArgNames []string // If the suggestion is a script, the args that the script takes.
	ArgKinds []cloudapipb.AutocompleteEntityKind
}

// TabStop represents a tab stop in a command.
type TabStop struct {
	Value          string
	Kind           cloudapipb.AutocompleteEntityKind
	Valid          bool
	Suggestions    []*Suggestion
	ContainsCursor bool
	ArgName        string
}

// Command represents an executable command.
type Command struct {
	TabStops   []*TabStop
	Executable bool
}

var kindLabelToProtoMap = map[string]cloudapipb.AutocompleteEntityKind{
	"svc":    cloudapipb.AEK_SVC,
	"pod":    cloudapipb.AEK_POD,
	"script": cloudapipb.AEK_SCRIPT,
	"ns":     cloudapipb.AEK_NAMESPACE,
}

var protoToKindLabelMap = map[cloudapipb.AutocompleteEntityKind]string{
	cloudapipb.AEK_SVC:       "svc",
	cloudapipb.AEK_POD:       "pod",
	cloudapipb.AEK_SCRIPT:    "script",
	cloudapipb.AEK_NAMESPACE: "ns",
}

// Autocomplete returns a formatted string and suggestions for the given input.
func Autocomplete(input string, cursorPos int, action cloudapipb.AutocompleteActionType, s Suggester, orgID uuid.UUID) (string, bool, []*cloudapipb.TabSuggestion, error) {
	inputWithCursor := input[:cursorPos] + "$0" + input[cursorPos:]
	cmd, err := ParseIntoCommand(inputWithCursor, s, orgID)
	if err != nil {
		return "", false, nil, err
	}

	fmtOutput, suggestions := cmd.ToFormatString(action)

	return fmtOutput, cmd.Executable, suggestions, nil
}

// ParseIntoCommand takes user input and attempts to parse it into a valid command with suggestions.
func ParseIntoCommand(input string, s Suggester, orgID uuid.UUID) (*Command, error) {
	parsedCmd, err := ebnf.ParseInput(input)
	if err != nil {
		return nil, err
	}

	cmd := &Command{}
	cmd.TabStops = make([]*TabStop, 0)

	action := "run"
	if parsedCmd.Action != nil {
		action = *parsedCmd.Action
	}
	// First tabstop should always be action.
	cmd.TabStops = append(cmd.TabStops, &TabStop{
		Value: action,
		Kind:  cloudapipb.AEK_UNKNOWN,
		Valid: true,
	})

	if action == "go" {
		err = parseGoCommand(parsedCmd, cmd, s)
	} else {
		err = parseRunCommand(parsedCmd, cmd, s, orgID)
	}

	if err != nil {
		return nil, err
	}

	return cmd, nil
}

func parseGoCommand(parsedCmd *ebnf.ParsedCmd, cmd *Command, s Suggester) error {
	return errors.New("Not yet implemented")
}

func parseRunScript(parsedCmd *ebnf.ParsedCmd, cmd *Command, s Suggester, orgID uuid.UUID) (int, bool, []string, []cloudapipb.AutocompleteEntityKind, error) {
	// The TabStop after the action should be the script. Check if there are any scripts defined.
	argNames := make([]string, 0)
	argTypes := make([]cloudapipb.AutocompleteEntityKind, 0)
	scriptTabIndex := -1
	scriptValid := false
	for i, a := range parsedCmd.Args {
		if a.Type != nil && *a.Type == "script" {
			// Determine if this is a valid script, and if so, what arguments it takes.
			input := ""
			if a.Name != nil {
				input = *a.Name
			}
			containsCursor := strings.Contains(input, CursorMarker)
			searchTerm := input
			if containsCursor {
				searchTerm = strings.Replace(searchTerm, CursorMarker, "", 1)
			}

			res, err := s.GetSuggestions([]*SuggestionRequest{&SuggestionRequest{orgID, searchTerm, []cloudapipb.AutocompleteEntityKind{cloudapipb.AEK_SCRIPT}, []cloudapipb.AutocompleteEntityKind{}}})
			if err != nil {
				return -1, scriptValid, nil, nil, err
			}

			suggestions := res[0].Suggestions
			exactMatch := res[0].ExactMatch

			if exactMatch {
				argNames = suggestions[0].ArgNames
				argTypes = suggestions[0].ArgKinds
				scriptValid = true
			}

			cmd.TabStops = append(cmd.TabStops, &TabStop{
				Value:          input,
				Kind:           cloudapipb.AEK_SCRIPT,
				Valid:          exactMatch,
				ContainsCursor: containsCursor,
			})

			scriptTabIndex = i
			break
		}
	}

	return scriptTabIndex, scriptValid, argNames, argTypes, nil
}

// parseRunArgsWithScript parses the command args according to the expected args for the given script.
func parseRunArgsWithScript(parsedCmd *ebnf.ParsedCmd, cmd *Command, s Suggester, argNames []string, argTypes []cloudapipb.AutocompleteEntityKind, scriptTabIndex int) ([]*TabStop, []cloudapipb.AutocompleteEntityKind) {
	// The tab stops for the command should be the args for the given script.
	argToTabStop := make(map[string]*TabStop)
	argNameToKind := make(map[string]cloudapipb.AutocompleteEntityKind)
	kindToArgNames := make(map[cloudapipb.AutocompleteEntityKind][]string)

	for i, n := range argNames {
		argToTabStop[n] = nil
		argType := argTypes[i]
		if _, ok := kindToArgNames[argType]; ok {
			kindToArgNames[argType] = append(kindToArgNames[argType], n)
		} else {
			kindToArgNames[argTypes[i]] = []string{n}
		}
		argNameToKind[n] = argType
	}

	unusedArgs := make([]*ebnf.ParsedArg, 0)
	// Look through args for any that are already labeled by an argName.
	for i, a := range parsedCmd.Args {
		if i == scriptTabIndex {
			continue // Don't re-parse the script field.
		}

		if a.Type != nil {
			if val, ok := argToTabStop[*a.Type]; ok {
				if val == nil { // No other cmdArg has been assigned to this scriptArg.
					label := *a.Type
					value := ""
					if a.Name != nil {
						value = *a.Name
					}
					argToTabStop[label] = &TabStop{
						Value:          value,
						Kind:           argNameToKind[label],
						ContainsCursor: strings.Contains(value, CursorMarker),
						ArgName:        label,
					}
					continue
				}
			}
		}
		unusedArgs = append(unusedArgs, a)
	}

	// Look through the remaining unassigned args for any that are labeled by entity type ("pod:", "svc:", etc.) and try to assign to a scriptArg with the same type.
	unassignedArgs := make([]string, 0)
	for _, a := range unusedArgs {
		if a.Type != nil {
			if val, ok := kindLabelToProtoMap[*a.Type]; ok { // The label is a specifier for the entity type.
				if argNames, argNamesOk := kindToArgNames[val]; argNamesOk {
					assigned := false
					for _, n := range argNames {
						// If any scriptArgs with this entityType is unassigned, assign this cmdArg to that scriptArg.
						if argToTabStop[n] == nil {
							value := ""
							if a.Name != nil {
								value = *a.Name
							}
							argToTabStop[n] = &TabStop{
								Value:          value,
								Kind:           val,
								ContainsCursor: strings.Contains(value, CursorMarker),
								ArgName:        n,
							}
							assigned = true
							break
						}
					}
					if assigned {
						continue
					}
				}
			}
		}

		value := ""
		if a.Type != nil {
			value += *a.Type + ":"
		}
		if a.Name != nil {
			value += *a.Name
		}
		unassignedArgs = append(unassignedArgs, value)
	}

	unassignedArgString := strings.Join(unassignedArgs, " ")

	// Create tabstops for any unassigne scriptArgs, and assign the remaining args to some unassigned scriptArg, if any.
	args := make([]*TabStop, 0)
	for _, n := range argNames {
		v := argToTabStop[n]
		if v != nil {
			args = append(args, v)
			continue
		}

		args = append(args, &TabStop{
			Value:          unassignedArgString,
			Kind:           argNameToKind[n],
			ArgName:        n,
			ContainsCursor: strings.Contains(unassignedArgString, CursorMarker),
		})
		unassignedArgString = ""
	}

	if unassignedArgString != "" { // If all of the scriptArgs were assigned, but there is a cmdArg remaining, create an additional tab stop.
		args = append(args, &TabStop{
			Value:          unassignedArgString,
			Kind:           cloudapipb.AEK_UNKNOWN,
			ContainsCursor: strings.Contains(unassignedArgString, CursorMarker),
		})
	}

	return args, make([]cloudapipb.AutocompleteEntityKind, 0)
}

// parseRunArgs parses the command args when no script is specified.
func parseRunArgs(parsedCmd *ebnf.ParsedCmd, cmd *Command, s Suggester, scriptTabIndex int) ([]*TabStop, []cloudapipb.AutocompleteEntityKind) {
	// Look through entities that are already defined (by svc:,pod:, etc. specifiers) to determine what arguments the
	// script needs to take.
	specifiedEntities := []cloudapipb.AutocompleteEntityKind{}
	args := make([]*TabStop, 0)
	for i, a := range parsedCmd.Args {
		if i == scriptTabIndex {
			continue // Don't re-parse the script field.
		}

		entityType := cloudapipb.AEK_UNKNOWN
		value := ""
		if a.Type != nil {
			if val, ok := kindLabelToProtoMap[*a.Type]; ok {
				entityType = val
			}
			value += *a.Type + ":"
		}

		if entityType != cloudapipb.AEK_UNKNOWN {
			value = ""
			specifiedEntities = append(specifiedEntities, entityType)
		}

		if a.Name != nil {
			value += *a.Name
		}

		args = append(args, &TabStop{
			Value:          value,
			Kind:           entityType,
			ContainsCursor: strings.Contains(value, CursorMarker),
		})
	}
	return args, specifiedEntities
}

func validateCommand(scriptDefined bool, cmd *Command) {
	// Determine if the command is executable.
	cmd.Executable = true
	if !scriptDefined {
		cmd.Executable = false
	}

	for _, a := range cmd.TabStops {
		// All args should be valid.
		if a.Valid != true {
			cmd.Executable = false
			break
		}
	}
}

func parseRunCommand(parsedCmd *ebnf.ParsedCmd, cmd *Command, s Suggester, orgID uuid.UUID) error {
	if parsedCmd.Args == nil {
		// TODO(michelle): Handle the case where there are no args.
		return nil
	}

	scriptTabIndex, scriptValid, argNames, argTypes, err := parseRunScript(parsedCmd, cmd, s, orgID)
	if err != nil {
		return err
	}

	var args []*TabStop
	var specifiedEntities []cloudapipb.AutocompleteEntityKind

	allowedKinds := []cloudapipb.AutocompleteEntityKind{cloudapipb.AEK_POD, cloudapipb.AEK_SVC, cloudapipb.AEK_NAMESPACE}
	if scriptValid {
		args, specifiedEntities = parseRunArgsWithScript(parsedCmd, cmd, s, argNames, argTypes, scriptTabIndex)
		allowedKinds = []cloudapipb.AutocompleteEntityKind{}
	} else {
		args, specifiedEntities = parseRunArgs(parsedCmd, cmd, s, scriptTabIndex)
		if scriptTabIndex == -1 {
			allowedKinds = append(allowedKinds, cloudapipb.AEK_SCRIPT)
		}
	}

	// Get suggestions for each argument.
	reqs := make([]*SuggestionRequest, 0)
	for _, a := range args {
		ak := allowedKinds
		if a.Kind != cloudapipb.AEK_UNKNOWN { // The kind is already specified in the input string.
			ak = []cloudapipb.AutocompleteEntityKind{a.Kind}
		}
		searchTerm := a.Value
		if a.ContainsCursor {
			searchTerm = strings.Replace(searchTerm, CursorMarker, "", 1)
		}
		reqs = append(reqs, &SuggestionRequest{orgID, searchTerm, ak, specifiedEntities})
	}

	res, err := s.GetSuggestions(reqs)
	if err != nil {
		return err
	}

	for i, a := range args {
		args[i].Suggestions = res[i].Suggestions
		args[i].Valid = res[i].ExactMatch && a.Kind != cloudapipb.AEK_UNKNOWN && a.ArgName != ""
	}

	cmd.TabStops = append(cmd.TabStops, args...)

	validateCommand(scriptTabIndex != -1, cmd)

	return nil
}

// ToFormatString converts a command to a formatted string with tab indexes, such as: ${1:run} ${2: px/svc_info}
func (cmd *Command) ToFormatString(action cloudapipb.AutocompleteActionType) (formattedInput string, suggestions []*cloudapipb.TabSuggestion) {
	curTabStop, nextInvalidTabStop, invalidTabs := cmd.processTabStops()

	// Move the cursor according to the action that was taken.
	switch action {
	case cloudapipb.AAT_EDIT:
		// If the action was an edit, the cursor should stay in the same position.
		break
	case cloudapipb.AAT_SELECT:
		// If the action was a select, we should move the cursor to the next invalid tabstop.
		// Remove cursor from current tab stop.
		cmd.TabStops[curTabStop].Value = strings.Replace(cmd.TabStops[curTabStop].Value, CursorMarker, "", 1)
		cmd.TabStops[nextInvalidTabStop].Value = cmd.TabStops[nextInvalidTabStop].Value + CursorMarker
		curTabStop = nextInvalidTabStop
		break
	default:
		break
	}

	// Construct the formatted string and tab suggestions.
	fStr := ""
	suggestions = make([]*cloudapipb.TabSuggestion, len(cmd.TabStops))
	for i, t := range cmd.TabStops {
		// The tab index of this tabstop is ((idx - (curTabStop + 1)) % numTabStops) + 1.
		idx := (((i - (curTabStop + 1)) + len(cmd.TabStops)) % len(cmd.TabStops)) + 1

		invalid, ok := invalidTabs[i]
		executableAfterSelect := ok && invalid && len(invalidTabs) == 1

		// Populate suggestions for the tab index.
		acSugg := make([]*cloudapipb.AutocompleteSuggestion, len(t.Suggestions))
		for j, s := range t.Suggestions {
			acSugg[j] = &cloudapipb.AutocompleteSuggestion{
				Kind:        s.Kind,
				Name:        s.Name,
				Description: s.Desc,
			}
		}

		ts := &cloudapipb.TabSuggestion{
			TabIndex:              int64(idx),
			ExecutableAfterSelect: executableAfterSelect,
			Suggestions:           acSugg,
		}
		suggestions[i] = ts

		// Append to the formatted string.
		if t.Value == "" && t.Kind == cloudapipb.AEK_UNKNOWN {
			fStr += fmt.Sprintf("$%d", idx)
		} else {
			fStr += fmt.Sprintf("${%d:", idx)
			if t.ArgName != "" {
				fStr += t.ArgName + ":"
			} else if t.Kind != cloudapipb.AEK_UNKNOWN {
				fStr += protoToKindLabelMap[t.Kind] + ":"
			}
			if t.Value != "" {
				fStr += t.Value
			}
			fStr += "}"
		}

		if i != len(cmd.TabStops)-1 {
			fStr += " "
		}
	}

	return fStr, suggestions
}

// processTabStops iterates through the tabs to determine which is the current tab the cursor is on, which is the next invalid
// tab stop that should be next if a selection is made, and finds all invalid tab stops.
func (cmd *Command) processTabStops() (int, int, map[int]bool) {
	// The tab stop contains the current cursor.
	curTabStop := -1
	// The next invalid tab stop in the command, that should be tabbed to if a selection is made.
	nextInvalidTabStop := -1
	// All tabs that are still invalid.
	invalidTabs := make(map[int]bool)
	for i, t := range cmd.TabStops {
		if !t.Valid {
			invalidTabs[i] = true

			if nextInvalidTabStop == -1 || (curTabStop != -1 && nextInvalidTabStop < curTabStop) {
				// If we haven't found an invalid tabstop, or if there is an invalid tabstop after the current tab stop, that should be the next tabstop.
				nextInvalidTabStop = i
			}
		}

		if t.ContainsCursor {
			curTabStop = i
		}
	}

	if curTabStop == -1 { // For some reason, if there is no cursor, we should put it at the very end.
		curTabStop = len(cmd.TabStops) - 1
		cmd.TabStops[curTabStop].Value = cmd.TabStops[curTabStop].Value + CursorMarker
	}
	if nextInvalidTabStop == -1 { // All tabstops are valid.
		nextInvalidTabStop = len(cmd.TabStops) - 1
	}
	return curTabStop, nextInvalidTabStop, invalidTabs
}
