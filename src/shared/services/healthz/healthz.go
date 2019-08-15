/**
package healthz defines health checkers and interfaces.
	By default it will install the ping checker at the /ping endpoint and
	passed in checkers at the /health/<checker_name> endpoint. Running /health will
	cause all checkers to be run.
*/

package healthz

import (
	"bytes"
	"fmt"
	"net/http"

	log "github.com/sirupsen/logrus"
)

// Checker is a named healthz checker.
type Checker interface {
	Name() string
	Check(req *http.Request) error
}

// PingHealthz returns true automatically when checked.
var PingHealthz Checker = ping{}

// mux is an interface describing the methods InstallHandler requires.
type mux interface {
	Handle(pattern string, handler http.Handler)
}

// RegisterPingEndpoint registers the ping endpoint to a serve mux.
func RegisterPingEndpoint(mux mux) {
	mux.Handle("/ping", adaptCheckToHandler(PingHealthz.Check))
}

// RegisterDefaultChecks register the default checks along with the passed in checks.
func RegisterDefaultChecks(mux mux, checks ...Checker) {
	RegisterPingEndpoint(mux)
	InstallPathHandler(mux, "/healthz", checks...)

}

// healthzCheck implements Checker on an arbitrary name and check function.
type healthzCheck struct {
	name  string
	check func(r *http.Request) error
}

var _ Checker = &healthzCheck{}

func (c *healthzCheck) Name() string {
	return c.name
}

func (c *healthzCheck) Check(r *http.Request) error {
	return c.check(r)
}

// NamedCheck returns a healthz checker for the given name and function.
func NamedCheck(name string, check func(r *http.Request) error) Checker {
	return &healthzCheck{name, check}
}

// InstallPathHandler registers the healtz checks under path.
// This function can only be called once per mux/path combo.
func InstallPathHandler(mux mux, path string, checks ...Checker) {
	if len(checks) == 0 {
		log.Debug("No default health checks specified. Installing the ping handler.")
		checks = []Checker{PingHealthz}
	}
	log.WithField("checkers", checkerNames(checks...)).Debug("Installing healthz checkers")
	mux.Handle(path, registerRootHealthzChecks(checks...))
	for _, check := range checks {
		mux.Handle(fmt.Sprintf("%s/%v", path, check.Name()), adaptCheckToHandler(check.Check))
	}
}

// adaptCheckToHandler returns an http.HandlerFunc that serves the provided checks.
func adaptCheckToHandler(c func(r *http.Request) error) http.HandlerFunc {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		err := c(r)
		if err != nil {
			http.Error(w, fmt.Sprintf("FAILED internal server error: %v", err), http.StatusInternalServerError)
		} else {
			fmt.Fprint(w, "OK")
		}
	})
}

func checkerNames(checks ...Checker) []string {
	if len(checks) > 0 {
		// accumulate the names of checks for printing them out.
		checkerNames := make([]string, 0, len(checks))
		for _, check := range checks {
			// quote the Name so we can disambiguate
			checkerNames = append(checkerNames, fmt.Sprintf("%q", check.Name()))
		}
		return checkerNames
	}
	return nil
}

func registerRootHealthzChecks(checks ...Checker) http.HandlerFunc {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		failed := false
		var verboseOut bytes.Buffer
		for _, check := range checks {
			if err := check.Check(r); err != nil {
				// don't include the error since this endpoint is public.  If someone wants more detail
				// they should have explicit permission to the detailed checks.
				log.WithField("checker", check.Name()).WithError(err).Error("healthz check failed")
				fmt.Fprintf(&verboseOut, "[-]%v FAILED: %v\n", check.Name(), err.Error())
				failed = true
			} else {
				fmt.Fprintf(&verboseOut, "[+]%v OK\n", check.Name())
			}
		}

		if failed {
			http.Error(w, fmt.Sprintf("FAILED\n%vhealthz check failed", verboseOut.String()),
				http.StatusInternalServerError)
			return
		}

		fmt.Fprint(w, "OK\n")
		verboseOut.WriteTo(w)
		fmt.Fprint(w, "healthz check passed\n")
	})
}

// ping implements the simplest possible healthz checker.
type ping struct{}

func (ping) Name() string {
	return "ping"
}

// PingHealthz is a health check that returns true.
func (ping) Check(_ *http.Request) error {
	return nil
}
