package cmd

import (
	"archive/tar"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"os/exec"
	"path"
	"strings"
	"time"

	"github.com/fatih/color"
	uuid "github.com/satori/go.uuid"
	"google.golang.org/grpc/metadata"
	"gopkg.in/segmentio/analytics-go.v3"
	"pixielabs.ai/pixielabs/src/shared/version"
	utils2 "pixielabs.ai/pixielabs/src/utils"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/artifacts"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/script"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/vizier"

	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/auth"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/pxanalytics"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/pxconfig"

	log "github.com/sirupsen/logrus"
	"github.com/spf13/cobra"
	"github.com/spf13/viper"
	"google.golang.org/grpc"
	v1 "k8s.io/api/core/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"pixielabs.ai/pixielabs/src/cloud/cloudapipb"

	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/certs"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/components"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/k8s"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/utils"
)

const (
	etcdYAMLPath            = "./yamls/vizier_deps/etcd_prod.yaml"
	natsYAMLPath            = "./yamls/vizier_deps/nats_prod.yaml"
	vizierBootstrapYAMLPath = "./yamls/vizier/vizier_bootstrap_prod.yaml"
)

// Sentry configs are not actually secret and safe to check in.
const (
	// We can't really distinguish between prod/dev, so we use some heuristics to decide.
	prodSentryDSN = "https://a8a635734bb840799befb63190e904e0@o324879.ingest.sentry.io/5203506"
	devSentryDSN  = "https://8e4acf22871543f1aa143a93a5216a16@o324879.ingest.sentry.io/5203508"
)

func getSentryDSN(vizierVersion string) string {
	// Only we have dev CLI.
	if version.GetVersion().IsDev() {
		return devSentryDSN
	}
	// If it contains - it must be a pre-release Vizier.
	if strings.Contains(vizierVersion, "-") {
		return devSentryDSN
	}
	return prodSentryDSN
}

// DeployCmd is the "deploy" command.
var DeployCmd = &cobra.Command{
	Use:   "deploy",
	Short: "Deploys Pixie on the current K8s cluster",
	PreRun: func(cmd *cobra.Command, args []string) {
		if e, has := os.LookupEnv("PL_VIZIER_VERSION"); has {
			viper.Set("use_version", e)
		}
	},
	PostRun: func(cmd *cobra.Command, args []string) {
		extractPath, _ := cmd.Flags().GetString("extract_yaml")
		if extractPath != "" {
			return
		}

		p := func(s string, a ...interface{}) {
			fmt.Fprintf(os.Stderr, s, a...)
		}
		u := color.New(color.Underline).Sprintf
		b := color.New(color.Bold)
		g := color.GreenString

		fmt.Fprint(os.Stderr, "\n")
		p(color.CyanString("==> ") + b.Sprint("Next Steps:\n"))
		p("\nVisit : %s to use Pixie's UI.\n", u("https://work.withpixie.ai"))
		p("\nRun some scripts using the %s cli. For example: \n", g("px"))
		p("- %s : to show pre-installed scripts.\n", g("px script list"))
		p("- %s : to run service info for sock-shop demo application (service selection coming soon!).\n",
			g("px run %s", script.ServiceStatsScript))
		p("\nCheck out our docs: %s.\n", u("https://work.withpixie.ai/docs"))
	},
	Run: runDeployCmd,
}

type taskWrapper struct {
	name string
	run  func() error
}

func newTaskWrapper(name string, run func() error) *taskWrapper {
	return &taskWrapper{
		name,
		run,
	}
}

func (t *taskWrapper) Name() string {
	return t.name
}

func (t *taskWrapper) Run() error {
	return t.run()
}

func init() {
	DeployCmd.Flags().StringP("extract_yaml", "e", "", "Directory to extract the Pixie yamls to")
	viper.BindPFlag("extract_yaml", DeployCmd.Flags().Lookup("extract_yaml"))

	DeployCmd.Flags().StringP("use_version", "v", "", "Pixie version to deploy")
	viper.BindPFlag("use_version", DeployCmd.Flags().Lookup("use_version"))

	DeployCmd.Flags().BoolP("check", "c", true, "Check whether the cluster can run Pixie")
	viper.BindPFlag("check", DeployCmd.Flags().Lookup("check"))

	DeployCmd.Flags().BoolP("check_only", "", false, "Only run check and exit.")
	viper.BindPFlag("check_only", DeployCmd.Flags().Lookup("check_only"))

	DeployCmd.Flags().StringP("credentials_file", "f", "", "Location of the Pixie credentials file")
	viper.BindPFlag("credentials_file", DeployCmd.Flags().Lookup("credentials_file"))

	DeployCmd.Flags().StringP("secret_name", "s", "pl-image-secret", "The name of the secret used to access the Pixie images")
	viper.BindPFlag("credentials_file", DeployCmd.Flags().Lookup("credentials_file"))

	DeployCmd.Flags().StringP("namespace", "n", "pl", "The namespace to install K8s secrets to")
	viper.BindPFlag("namespace", DeployCmd.Flags().Lookup("namespace"))

	DeployCmd.Flags().BoolP("deps_only", "d", false, "Deploy only the cluster dependencies, not the agents")
	viper.BindPFlag("deps_only", DeployCmd.Flags().Lookup("deps_only"))

	DeployCmd.Flags().StringP("dev_cloud_namespace", "m", "", "The namespace of Pixie Cloud, if running Cloud on minikube")
	viper.BindPFlag("dev_cloud_namespace", DeployCmd.Flags().Lookup("dev_cloud_namespace"))

	DeployCmd.Flags().StringP("deploy_key", "k", "", "The deploy key to use to deploy Pixie")
	viper.BindPFlag("deploy_key", DeployCmd.Flags().Lookup("deploy_key"))

	// Super secret flags for Pixies.
	DeployCmd.Flags().MarkHidden("namespace")
	DeployCmd.Flags().MarkHidden("dev_cloud_namespace")
}

func newVizAuthClient(conn *grpc.ClientConn) cloudapipb.VizierImageAuthorizationClient {
	return cloudapipb.NewVizierImageAuthorizationClient(conn)
}

func newArtifactTrackerClient(conn *grpc.ClientConn) cloudapipb.ArtifactTrackerClient {
	return cloudapipb.NewArtifactTrackerClient(conn)
}

func mustGetImagePullSecret(conn *grpc.ClientConn) string {
	// Make rpc request to the cloud to get creds.
	client := newVizAuthClient(conn)
	creds, err := auth.LoadDefaultCredentials()
	if err != nil {
		log.WithError(err).Fatal("Failed to get creds. You might have to run: 'pixie auth login'")
	}
	req := &cloudapipb.GetImageCredentialsRequest{}
	ctxWithCreds := metadata.AppendToOutgoingContext(context.Background(), "authorization",
		fmt.Sprintf("bearer %s", creds.Token))

	resp, err := client.GetImageCredentials(ctxWithCreds, req)
	if err != nil {
		log.WithError(err).Fatal("Failed to fetch image credentials")
	}
	return resp.Creds
}

func mustReadCredsFile(credsFile string) string {
	credsData, err := ioutil.ReadFile(credsFile)
	if err != nil {
		log.WithError(err).Fatal(fmt.Sprintf("Could not read file: %s", credsFile))
	}
	return string(credsData)
}

func getLatestVizierVersion(conn *grpc.ClientConn) (string, error) {
	client := newArtifactTrackerClient(conn)

	creds, err := auth.LoadDefaultCredentials()
	if err != nil {
		return "", err
	}

	req := &cloudapipb.GetArtifactListRequest{
		ArtifactName: "vizier",
		ArtifactType: cloudapipb.AT_CONTAINER_SET_YAMLS,
		Limit:        1,
	}
	ctxWithCreds := metadata.AppendToOutgoingContext(context.Background(), "authorization",
		fmt.Sprintf("bearer %s", creds.Token))

	resp, err := client.GetArtifactList(ctxWithCreds, req)
	if err != nil {
		return "", err
	}

	if len(resp.Artifact) != 1 {
		return "", errors.New("Could not find Vizier artifact")
	}

	return resp.Artifact[0].VersionStr, nil
}

func runDeployCmd(cmd *cobra.Command, args []string) {
	check, _ := cmd.Flags().GetBool("check")
	checkOnly, _ := cmd.Flags().GetBool("check_only")
	extractPath, _ := cmd.Flags().GetString("extract_yaml")
	deployKey, _ := cmd.Flags().GetString("deploy_key")

	if deployKey == "" && extractPath != "" {
		log.Fatal("--deploy_key must be specified when running with --extract_yaml. Please run px deploy-key create.")
	}

	if (check || checkOnly) && extractPath == "" {
		_ = pxanalytics.Client().Enqueue(&analytics.Track{
			UserId: pxconfig.Cfg().UniqueClientID,
			Event:  "Cluster Check Run",
		})

		err := k8s.RunDefaultClusterChecks()
		if err != nil {
			_ = pxanalytics.Client().Enqueue(&analytics.Track{
				UserId: pxconfig.Cfg().UniqueClientID,
				Event:  "Cluster Check Failed",
				Properties: analytics.NewProperties().
					Set("error", err.Error()),
			})
			log.WithError(err).Fatalln("Check pre-check has failed. To by pass pass in --check=false.")
		}

		if checkOnly {
			fmt.Print("\nAll Checks Passed!\n")
			os.Exit(0)
		}
	}

	namespace, _ := cmd.Flags().GetString("namespace")
	credsFile, _ := cmd.Flags().GetString("credentials_file")
	devCloudNS := viper.GetString("dev_cloud_namespace")

	cloudAddr := viper.GetString("cloud_addr")

	// Get grpc connection to cloud.
	cloudConn, err := utils.GetCloudClientConnection(cloudAddr)
	if err != nil {
		log.Fatalln(err)
	}

	versionString := viper.GetString("use_version")
	inputVersionStr := versionString
	if len(versionString) == 0 {
		// Fetch latest version.
		versionString, err = getLatestVizierVersion(cloudConn)
		if err != nil {
			log.WithError(err).Fatal("Failed to fetch Vizier versions")
		}
	}
	fmt.Printf("Installing version: %s\n", versionString)

	// Get deploy key, if not already specified.
	deployKeyID := ""
	if deployKey == "" {
		deployKeyID, deployKey, err = generateDeployKey(cloudAddr, "Auto-generated by the Pixie CLI")
		if err != nil {
			log.WithError(err).Fatal("Failed to generate deployment key")
		}
		defer deleteDeployKey(cloudAddr, uuid.FromStringOrNil(deployKeyID))
	}

	kubeConfig := k8s.GetConfig()

	fmt.Printf("Generating YAMLs for Pixie\n")

	yamlMap := make(map[string]string)
	var vzYamlMap map[string]string
	nsYAMLPath := ""

	// Generate YAMLs.
	yamlIdx := 0 // This is used to track the order in which YAMLs should be applied.
	nsYamlJob := newTaskWrapper("Generating namespace YAML", func() error {
		nsYAML, err := generateNamespaceYAML(namespace)
		if err != nil {
			return err
		}
		nsYAMLPath = fmt.Sprintf("./pixie_yamls/00_namespace/%02d_namespace.yaml", yamlIdx)
		yamlMap[nsYAMLPath] = nsYAML

		yamlIdx++
		return nil
	})
	certsYamlJob := newTaskWrapper("Generating cert YAMLs", func() error {
		certYAMLs, err := certs.DefaultGenerateCertYAMLs(namespace)
		if err != nil {
			return err
		}
		yamlMap[fmt.Sprintf("./pixie_yamls/01_secrets/%02d_cert.yaml", yamlIdx)] = certYAMLs

		yamlIdx++
		return nil
	})
	secretsYamlJob := newTaskWrapper("Generating secret YAMLs", func() error {
		var credsData string
		if credsFile == "" {
			credsData = mustGetImagePullSecret(cloudConn)
		} else {
			credsData = mustReadCredsFile(credsFile)
		}
		secretName, _ := cmd.Flags().GetString("secret_name")
		dockerSecret, err := k8s.CreateDockerConfigJSONSecret(namespace, secretName, credsData)
		if err != nil {
			return err
		}
		dYaml, err := k8s.ConvertResourceToYAML(dockerSecret)
		if err != nil {
			return err
		}
		yamlMap[fmt.Sprintf("./pixie_yamls/01_secrets/%02d_secret.yaml", yamlIdx)] = dYaml
		yamlIdx++

		kConfig := kubeConfig
		if extractPath != "" {
			kConfig = nil
		}

		csYAMLs, err := GenerateClusterSecretYAMLs(cloudAddr, deployKey, namespace, devCloudNS, kConfig, getSentryDSN(versionString), inputVersionStr)
		if err != nil {
			return err
		}
		yamlMap[fmt.Sprintf("./pixie_yamls/01_secrets/%02d_secret.yaml", yamlIdx)] = csYAMLs

		yamlIdx++

		return nil
	})
	vzYamlJob := newTaskWrapper("Downloading Vizier YAMLs", func() error {
		creds, err := auth.LoadDefaultCredentials()
		if err != nil {
			return err
		}
		vzYamlMap, err = artifacts.FetchVizierYAMLMap(cloudConn, creds.Token, versionString)
		if err != nil {
			return err
		}
		return nil
	})

	yamlJobs := []utils.Task{nsYamlJob, certsYamlJob, secretsYamlJob, vzYamlJob}
	jr := utils.NewSerialTaskRunner(yamlJobs)
	err = jr.RunAndMonitor()
	if err != nil {
		_ = pxanalytics.Client().Enqueue(&analytics.Track{
			UserId: pxconfig.Cfg().UniqueClientID,
			Event:  "Deploy Failure",
			Properties: analytics.NewProperties().
				Set("err", err.Error()),
		})
		log.Fatal("Failed to generate YAMLs for deploy")
	}

	// If extract_path is specified, write out yamls to file.
	if extractPath != "" {

		writeYAML := func(w *tar.Writer, name string, contents string) error {
			if err = w.WriteHeader(&tar.Header{Name: name, Size: int64(len(contents)), Mode: 777}); err != nil {
				return err
			}
			if _, err = w.Write([]byte(contents)); err != nil {
				return err
			}
			return nil
		}

		filePath := path.Join(extractPath, "yamls.tar")
		writer, err := os.OpenFile(filePath, os.O_RDWR|os.O_CREATE, 0755)
		if err != nil {
			log.WithError(err).Fatal("Failed to open file to write YAMLs")
		}
		defer writer.Close()
		w := tar.NewWriter(writer)

		for k, v := range yamlMap {
			err := writeYAML(w, k, v)
			if err != nil {
				log.WithError(err).Fatal("Failed to write YAMLs")
			}
		}

		// Write etcd + nats before bootstrap YAML.
		err = writeYAML(w, fmt.Sprintf("./pixie_yamls/02_manifests/%02d_etcd.yaml", yamlIdx), vzYamlMap[etcdYAMLPath])
		if err != nil {
			log.WithError(err).Fatal("Failed to write YAMLs")
		}
		yamlIdx++

		err = writeYAML(w, fmt.Sprintf("./pixie_yamls/02_manifests/%02d_nats.yaml", yamlIdx), vzYamlMap[natsYAMLPath])
		if err != nil {
			log.WithError(err).Fatal("Failed to write YAMLs")
		}
		yamlIdx++

		err = writeYAML(w, fmt.Sprintf("./pixie_yamls/02_manifests/%02d_bootstrap.yaml", yamlIdx), vzYamlMap[vizierBootstrapYAMLPath])
		if err != nil {
			log.WithError(err).Fatal("Failed to write YAMLs")
		}

		if err = w.Close(); err != nil {
			if err != nil {
				log.WithError(err).Fatal("Failed to write YAMLs")
			}
		}

		return
	}

	_ = pxanalytics.Client().Enqueue(&analytics.Track{
		UserId: pxconfig.Cfg().UniqueClientID,
		Event:  "Deploy Initiated",
		Properties: analytics.NewProperties().
			Set("cloud_addr", cloudAddr),
	})

	_ = pxanalytics.Client().Enqueue(&analytics.Track{
		UserId: pxconfig.Cfg().UniqueClientID,
		Event:  "Deploy Started",
		Properties: analytics.NewProperties().
			Set("cloud_addr", cloudAddr),
	})

	currentCluster := getCurrentCluster()
	fmt.Printf("Deploying Pixie to the following cluster: %s\n", currentCluster)
	clusterOk := components.YNPrompt("Is the cluster correct?", true)
	if !clusterOk {
		fmt.Printf("Cluster is not correct. Aborting.")
		return
	}

	clientset := k8s.GetClientset(kubeConfig)
	od := k8s.ObjectDeleter{
		Namespace:  namespace,
		Clientset:  clientset,
		RestConfig: kubeConfig,
		Timeout:    2 * time.Minute,
	}
	// Get the number of nodes.
	numNodes, err := getNumNodes(clientset)
	if err != nil {
		fmt.Println(err.Error())
	}

	fmt.Printf("Found %v nodes\n", numNodes)

	namespaceJob := newTaskWrapper("Creating namespace", func() error {
		return k8s.ApplyYAML(clientset, kubeConfig, namespace, strings.NewReader(yamlMap[nsYAMLPath]))
	})

	clusterRoleJob := newTaskWrapper("Deleting stale Pixie objects, if any", func() error {
		// TODO(zasgar/michelle): Only run this if we see stale objects.
		_, err := od.DeleteByLabel("component=vizier", k8s.AllResourceKinds...)
		return err
	})

	certJob := newTaskWrapper("Deploying certs, secrets, and configmaps", func() error {
		for k, v := range yamlMap {
			if k == nsYAMLPath {
				continue
			}
			err := k8s.ApplyYAML(clientset, kubeConfig, namespace, strings.NewReader(v))
			if err != nil {
				return err
			}
		}
		return nil
	})

	setupJobs := []utils.Task{
		namespaceJob, clusterRoleJob, certJob}
	jr = utils.NewSerialTaskRunner(setupJobs)
	err = jr.RunAndMonitor()
	if err != nil {
		_ = pxanalytics.Client().Enqueue(&analytics.Track{
			UserId: pxconfig.Cfg().UniqueClientID,
			Event:  "Deploy Failure",
			Properties: analytics.NewProperties().
				Set("err", err.Error()),
		})
		log.Fatal("Failed to deploy Vizier")
	}

	depsOnly, _ := cmd.Flags().GetBool("deps_only")

	clusterID := deploy(cloudConn, versionString, clientset, kubeConfig, vzYamlMap, namespace, depsOnly)

	waitForHealthCheck(cloudAddr, clusterID, clientset, namespace, numNodes)
}

func runSimpleHealthCheckScript(cloudAddr string, clusterID uuid.UUID) error {
	v, err := vizier.ConnectionToVizierByID(cloudAddr, clusterID)
	br := mustCreateBundleReader()
	if err != nil {
		return err
	}
	execScript := br.MustGetScript(script.AgentStatusScript)

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	resp, err := v.ExecuteScriptStream(ctx, execScript)
	if err != nil {
		return err
	}

	// TODO(zasgar): Make this use the Null output. We can't right now
	// because of fatal message on vizier failure.
	errCh := make(chan error)
	// Eat all responses.
	go func() {
		for {
			select {
			case <-ctx.Done():
				if ctx.Err() != nil {
					errCh <- ctx.Err()
					return
				}
				errCh <- nil
				return
			case msg := <-resp:
				if msg == nil {
					errCh <- nil
				}
				if msg.Err != nil {
					if msg.Err == io.EOF {
						errCh <- nil
						return
					}
					errCh <- msg.Err
					return
				}
				if msg.Resp.Status != nil && msg.Resp.Status.Code != 0 {
					errCh <- errors.New(msg.Resp.Status.Message)
				}
				// Eat messages.
			}
		}
	}()

	err = <-errCh
	return err
}

func waitForHealthCheckTaskGenerator(cloudAddr string, clusterID uuid.UUID) func() error {
	return func() error {
		timeout := time.NewTimer(2 * time.Minute)
		defer timeout.Stop()
		for {
			select {
			case <-timeout.C:
				return errors.New("timeout waiting for healthcheck")
			default:
				err := runSimpleHealthCheckScript(cloudAddr, clusterID)
				if err == nil {
					return nil
				}
				time.Sleep(5 * time.Second)
			}
		}
	}
}

func waitForHealthCheck(cloudAddr string, clusterID uuid.UUID, clientset *kubernetes.Clientset, namespace string, numNodes int) {
	fmt.Printf("Waiting for Pixie to pass healthcheck\n")

	healthCheckJobs := []utils.Task{
		newTaskWrapper("Wait for PEMs/Kelvin", func() error {
			return waitForPems(clientset, namespace, numNodes)
		}),
		newTaskWrapper("Wait for healthcheck", waitForHealthCheckTaskGenerator(cloudAddr, clusterID)),
	}

	hc := utils.NewSerialTaskRunner(healthCheckJobs)
	err := hc.RunAndMonitor()
	if err != nil {
		_ = pxanalytics.Client().Enqueue(&analytics.Track{
			UserId: pxconfig.Cfg().UniqueClientID,
			Event:  "Deploy Healthcheck Failed",
			Properties: analytics.NewProperties().
				Set("err", err.Error()),
		})
		log.WithError(err).Fatal("Failed Pixie healthcheck")
	}
	_ = pxanalytics.Client().Enqueue(&analytics.Track{
		UserId: pxconfig.Cfg().UniqueClientID,
		Event:  "Deploy Healthcheck Passed",
	})
}
func getCurrentCluster() string {
	kcmd := exec.Command("kubectl", "config", "current-context")
	var out bytes.Buffer
	kcmd.Stdout = &out
	kcmd.Stderr = os.Stderr
	err := kcmd.Run()

	if err != nil {
		log.WithError(err).Fatal("Error getting current kubernetes cluster")
	}
	return out.String()
}

func generateNamespaceYAML(namespace string) (string, error) {
	ns := &v1.Namespace{}
	ns.SetGroupVersionKind(v1.SchemeGroupVersion.WithKind("Namespace"))
	ns.Name = namespace

	return k8s.ConvertResourceToYAML(ns)
}

func waitForCluster(ctx context.Context, conn *grpc.ClientConn, clusterID *uuid.UUID) error {
	client := cloudapipb.NewVizierClusterInfoClient(conn)

	creds, err := auth.LoadDefaultCredentials()
	if err != nil {
		return err
	}

	req := &cloudapipb.GetClusterInfoRequest{
		ID: utils2.ProtoFromUUID(clusterID),
	}

	ctxWithCreds := metadata.AppendToOutgoingContext(context.Background(), "authorization",
		fmt.Sprintf("bearer %s", creds.Token))

	t := time.NewTicker(2 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-t.C:
			resp, err := client.GetClusterInfo(ctxWithCreds, req)
			if err != nil {
				return err
			}
			if len(resp.Clusters) > 0 && resp.Clusters[0].Status != cloudapipb.CS_DISCONNECTED {
				return nil
			}
		case <-ctx.Done():
			return errors.New("context cancelled waiting for cluster to come online")
		}
	}
}

func initiateUpdate(ctx context.Context, conn *grpc.ClientConn, clusterID *uuid.UUID, version string, redeployEtcd bool) error {
	client := cloudapipb.NewVizierClusterInfoClient(conn)

	creds, err := auth.LoadDefaultCredentials()
	if err != nil {
		return err
	}

	req := &cloudapipb.UpdateOrInstallClusterRequest{
		ClusterID:    utils2.ProtoFromUUID(clusterID),
		Version:      version,
		RedeployEtcd: redeployEtcd,
	}

	ctxWithCreds := metadata.AppendToOutgoingContext(ctx, "authorization",
		fmt.Sprintf("bearer %s", creds.Token))

	resp, err := client.UpdateOrInstallCluster(ctxWithCreds, req)
	if err != nil {
		return err
	}
	if !resp.UpdateStarted {
		return errors.New("failed to start install process")
	}
	return nil
}

func deploy(cloudConn *grpc.ClientConn, version string, clientset *kubernetes.Clientset, config *rest.Config, yamlMap map[string]string, namespace string, depsOnly bool) uuid.UUID {
	// NATS and etcd deploys depend on timing, so may sometimes fail. Include some retry behavior.
	// TODO(zasgar/michelle): This logic is flaky and we should make smarter to actually detect and wait
	// based on the message.
	natsJob := newTaskWrapper("Deploying NATS", func() error {
		return retryDeploy(clientset, config, namespace, yamlMap[natsYAMLPath])
	})

	etcdJob := newTaskWrapper("Deploying etcd", func() error {
		return retryDeploy(clientset, config, namespace, yamlMap[etcdYAMLPath])
	})

	deployDepsJobs := []utils.Task{natsJob, etcdJob}

	jr := utils.NewParallelTaskRunner(deployDepsJobs)
	err := jr.RunAndMonitor()
	if err != nil {
		log.Fatal("Failed to deploy Vizier deps")
	}

	if depsOnly {
		return uuid.Nil
	}
	var clusterID uuid.UUID
	deployJob := []utils.Task{
		newTaskWrapper("Deploying Cloud Connector", func() error {
			return k8s.ApplyYAML(clientset, config, namespace, strings.NewReader(yamlMap[vizierBootstrapYAMLPath]))
		}),
		newTaskWrapper("Waiting for Cloud Connector to come online", func() error {
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
			defer cancel()

			t := time.NewTicker(2 * time.Second)
			defer t.Stop()
			clusterIDExists := false

			for !clusterIDExists { // Wait for secret to be updated with clusterID.
				select {
				case <-ctx.Done():
					log.Fatal("Timed out waiting for cluster ID assignment")
				case <-t.C:
					s := k8s.GetSecret(clientset, namespace, "pl-cluster-secrets")
					if cID, ok := s.Data["cluster-id"]; ok {
						clusterID = uuid.FromStringOrNil(string(cID))
						clusterIDExists = true
					}
				}
			}

			return waitForCluster(ctx, cloudConn, &clusterID)
		}),
	}

	vzJr := utils.NewSerialTaskRunner(deployJob)
	err = vzJr.RunAndMonitor()
	if err != nil {
		log.Fatal("Failed to deploy Vizier")
	}
	return clusterID
}

func retryDeploy(clientset *kubernetes.Clientset, config *rest.Config, namespace string, yamlContents string) error {
	tries := 12
	var err error
	for tries > 0 {
		err = k8s.ApplyYAML(clientset, config, namespace, strings.NewReader(yamlContents))
		if err == nil {
			return nil
		}

		time.Sleep(5 * time.Second)
		tries--
	}
	if tries == 0 {
		return err
	}
	return nil
}

func isPodUnschedulable(podStatus *v1.PodStatus) bool {
	for _, cond := range podStatus.Conditions {
		if cond.Reason == "Unschedulable" {
			return true
		}
	}
	return false
}

func podUnschedulableMessage(podStatus *v1.PodStatus) string {
	for _, cond := range podStatus.Conditions {
		if cond.Reason == "Unschedulable" {
			return cond.Message
		}
	}
	return ""
}

func pemCanScheduleWithTaint(t *v1.Taint) bool {
	// For now an effect of NoSchedule should be sufficient, we don't have tolerations in the Daemonset spec.
	if t.Effect == "NoSchedule" {
		return false
	}
	return true
}

func getNumNodes(clientset *kubernetes.Clientset) (int, error) {
	nodes, err := k8s.ListNodes(clientset)
	if err != nil {
		return 0, err
	}
	unscheduleableNodes := 0
	for _, n := range nodes.Items {
		for _, t := range n.Spec.Taints {
			if !pemCanScheduleWithTaint(&t) {
				unscheduleableNodes++
				break
			}
		}
	}
	return len(nodes.Items) - unscheduleableNodes, nil
}

var empty struct{}

// waitForPems waits for the Vizier's Proxy service to be ready with an external IP.
func waitForPems(clientset *kubernetes.Clientset, namespace string, expectedPods int) error {
	// Watch for pod updates.
	watcher, err := k8s.WatchK8sResource(clientset, "pods", namespace)
	if err != nil {
		return err
	}

	failedSchedulingPods := make(map[string]string)
	successfulPods := make(map[string]struct{})
	for c := range watcher.ResultChan() {
		pod := c.Object.(*v1.Pod)
		name, ok := pod.Labels["name"]
		if !ok {
			continue
		}

		// Skip any pods that are not vizier-pems.
		if name != "vizier-pem" {
			continue
		}

		switch pod.Status.Phase {
		case "Pending":
			if isPodUnschedulable(&pod.Status) {
				failedSchedulingPods[pod.Name] = podUnschedulableMessage(&pod.Status)
			}

		case "Running":
			successfulPods[pod.Name] = empty
		default:
			// TODO(philkuz/zasgar) should we make this a print line instead?
			return fmt.Errorf("unexpected status for PEM '%s': '%v'", pod.Name, pod.Status.Phase)
		}

		if len(successfulPods) == expectedPods {
			return nil
		}
		if len(successfulPods)+len(failedSchedulingPods) == expectedPods {
			failedPems := make([]string, 0)
			for k, v := range failedSchedulingPods {
				failedPems = append(failedPems, fmt.Sprintf("'%s': '%s'", k, v))
			}

			return fmt.Errorf("Failed to schedule pems:\n%s", strings.Join(failedPems, "\n"))
		}
	}
	return nil
}
