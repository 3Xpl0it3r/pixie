package controllers

import (
	"errors"
	"fmt"
	"strconv"
	"strings"

	"github.com/spf13/cast"
	planpb "pixielabs.ai/pixielabs/src/carnot/planpb"
)

// The prefix which a PL Config line should begin with.
const plConfigPrefix = "#px:set "

// Default values for config flags. If a flag is not included in this map,
// it is not considered a valid flag that can be set.
var defaultQueryFlags = map[string]interface{}{
	"explain": false,
	"analyze": false,
}

// QueryFlags represents a set of Pixie configuration flags.
type QueryFlags struct {
	flags map[string]interface{}
}

func newQueryFlags() *QueryFlags {
	return &QueryFlags{
		flags: map[string]interface{}{},
	}
}

// get gets the value of the given flag. It returns the default value if the flag
// has not been set.
func (f *QueryFlags) get(key string) interface{} {
	// Check to see if the key is in the map.
	if val, ok := f.flags[key]; ok {
		return val
	}
	// Check if the key is defined in the default configs.
	if val, ok := defaultQueryFlags[key]; ok {
		return val
	}
	return nil
}

// GetInt gets the value of the given flag as an int.
func (f *QueryFlags) GetInt(key string) int {
	val := f.get(key)
	return cast.ToInt(val)
}

// GetBool gets the value of the given flag as a bool.
func (f *QueryFlags) GetBool(key string) bool {
	val := f.get(key)
	return cast.ToBool(val)
}

// GetString gets the value of the given flag as a string.
func (f *QueryFlags) GetString(key string) string {
	val := f.get(key)
	return cast.ToString(val)
}

// GetFloat64 gets the value of the given flag as a float.
func (f *QueryFlags) GetFloat64(key string) float64 {
	val := f.get(key)
	return cast.ToFloat64(val)
}

func (f *QueryFlags) set(key string, value string) error {
	// Ensure that the key is a valid flag that can be set, by checking it is
	// defined in the defaults.
	if defVal, ok := defaultQueryFlags[key]; ok {
		var typedVal interface{}
		var err error
		switch defVal.(type) {
		case int:
			typedVal, err = strconv.ParseInt(value, 10, 64)
			break
		case float64:
			typedVal, err = strconv.ParseFloat(value, 64)
			break
		case string:
			typedVal = value
			break
		case bool:
			typedVal, err = strconv.ParseBool(value)
			break
		}

		if err != nil {
			return err
		}
		f.flags[key] = typedVal
		return nil
	}

	return fmt.Errorf("%s is not a valid flag", key)
}

// GetPlanOptions creates the plan option proto from the specified query flags.
func (f *QueryFlags) GetPlanOptions() *planpb.PlanOptions {
	return &planpb.PlanOptions{
		Explain: f.GetBool("explain"),
		Analyze: f.GetBool("analyze"),
	}
}

// ParseQueryFlags takes a query string containing some config options and generates
// a QueryFlags object that can be used to retrieve those options.
func ParseQueryFlags(queryStr string) (*QueryFlags, error) {
	qf := newQueryFlags()

	for _, line := range strings.Split(strings.TrimSuffix(queryStr, "\n"), "\n") {
		// If the line begins with the PL config prefix, attempt to parse the line.
		if strings.HasPrefix(line, plConfigPrefix) {
			queryComponents := strings.Split(line, " ")

			if len(queryComponents) != 2 {
				return nil, errors.New("Config setting is malformed")
			}
			keyVal := strings.Split(queryComponents[1], "=")
			if len(keyVal) != 2 {
				return nil, errors.New("Config setting is malformed")
			}
			err := qf.set(keyVal[0], keyVal[1])
			if err != nil {
				return nil, err
			}
		}
	}

	return qf, nil
}
