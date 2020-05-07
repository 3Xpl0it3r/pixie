package cmd

import (
	log "github.com/sirupsen/logrus"
	"github.com/spf13/cobra"
	"github.com/spf13/viper"
	"pixielabs.ai/pixielabs/src/cloud/cloudapipb"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/live"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/script"
)

func init() {
	LiveCmd.Flags().StringP("bundle", "b", "", "Path/URL to bundle file")
	LiveCmd.Flags().StringP("file", "f", "", "Script file, specify - for STDIN")
	LiveCmd.Flags().BoolP("new_autocomplete", "n", false, "Whether to use the new autocomplete")
}

// LiveCmd is the "query" command.
var LiveCmd = &cobra.Command{
	Use:   "live",
	Short: "Interactive Pixie Views",
	Run: func(cmd *cobra.Command, args []string) {
		cloudAddr := viper.GetString("cloud_addr")

		useNewAC, _ := cmd.Flags().GetBool("new_autocomplete")

		br := mustCreateBundleReader()
		var execScript *script.ExecutableScript
		var err error
		scriptFile, _ := cmd.Flags().GetString("file")
		if len(args) > 0 {
			scriptName := args[0]
			execScript = br.MustGetScript(scriptName)
			fs := execScript.GetFlagSet()

			if fs != nil {
				if err := fs.Parse(args[1:]); err != nil {
					log.WithError(err).Fatal("Failed to parse script flags")
				}
				execScript.UpdateFlags(fs)
			}
		} else if len(scriptFile) > 0 {
			execScript, err = loadScriptFromFile(scriptFile)
			if err != nil {
				log.WithError(err).Fatal("Failed to get query string")
			}
			// Add custom script to bundle reader so we can access it later.
			br.AddScript(execScript)
		}

		cloudConn, err := getCloudClientConnection(cloudAddr)
		if err != nil {
			log.WithError(err).Fatal("Could not connect to cloud")
		}
		aClient := cloudapipb.NewAutocompleteServiceClient(cloudConn)

		v := mustConnectDefaultVizier(cloudAddr)

		lv, err := live.New(br, v, aClient, execScript, useNewAC)
		if err != nil {
			log.WithError(err).Fatal("Failed to initialize live view")
		}

		if err := lv.Run(); err != nil {
			log.WithError(err).Fatal("Failed to run live view")
		}
	},
}
