package main

import (
	"net/http"

	"github.com/nats-io/nats.go"
	"github.com/nats-io/stan.go"
	uuid "github.com/satori/go.uuid"
	"github.com/spf13/pflag"
	"github.com/spf13/viper"
	"google.golang.org/grpc"
	"pixielabs.ai/pixielabs/src/cloud/vzconn/bridge"
	"pixielabs.ai/pixielabs/src/cloud/vzconn/vzconnpb"
	"pixielabs.ai/pixielabs/src/cloud/vzmgr/vzmgrpb"
	"pixielabs.ai/pixielabs/src/shared/services/env"

	log "github.com/sirupsen/logrus"
	"pixielabs.ai/pixielabs/src/shared/services"
	"pixielabs.ai/pixielabs/src/shared/services/healthz"
)

func init() {
	pflag.String("vzmgr_service", "kubernetes:///vzmgr-service.plc:51800", "The profile service url (load balancer/list is ok)")
	pflag.String("nats_url", "pl-nats", "The URL of NATS")
	pflag.String("stan_cluster", "pl-stan", "The name of the Stan cluster")
}

func newVZMgrServiceClient() (vzmgrpb.VZMgrServiceClient, error) {
	dialOpts, err := services.GetGRPCClientDialOpts()
	if err != nil {
		return nil, err
	}

	vzmgrChannel, err := grpc.Dial(viper.GetString("vzmgr_service"), dialOpts...)
	if err != nil {
		return nil, err
	}

	return vzmgrpb.NewVZMgrServiceClient(vzmgrChannel), nil
}

func createStanNatsConnection(clientID string) (nc *nats.Conn, sc stan.Conn, err error) {
	nc, err = nats.Connect(viper.GetString("nats_url"),
		nats.ClientCert(viper.GetString("client_tls_cert"), viper.GetString("client_tls_key")),
		nats.RootCAs(viper.GetString("tls_ca_cert")))
	if err != nil {
		return
	}
	sc, err = stan.Connect(viper.GetString("stan_cluster"), clientID, stan.NatsConn(nc))
	return
}

func main() {
	log.WithField("service", "vzconn-service").Info("Starting service")

	services.SetupService("vzconn-service", 51600)
	services.PostFlagSetupAndParse()
	services.CheckServiceFlags()
	services.SetupServiceLogging()

	mux := http.NewServeMux()
	healthz.RegisterDefaultChecks(mux)
	// VZConn is the backend for a GCLB and that health checks on "/" instead of the regular health check endpoint.
	healthz.InstallPathHandler(mux, "/")

	// Communication from Vizier to VZConn is not auth'd via GRPC auth.
	serverOpts := &services.GRPCServerOptions{
		DisableAuth: map[string]bool{
			"/pl.services.VZConnService/NATSBridge":               true,
			"/pl.services.VZConnService/RegisterVizierDeployment": true,
		},
	}

	s := services.NewPLServerWithOptions(env.New(), mux, serverOpts)
	nc, sc, err := createStanNatsConnection(uuid.NewV4().String())
	if err != nil {
		log.WithError(err).Error("Could not connect to Nats/Stan")
	}

	nc.SetErrorHandler(func(conn *nats.Conn, subscription *nats.Subscription, err error) {
		log.WithField("Sub", subscription.Subject).
			WithError(err).
			Error("Error with NATS handler")
	})

	vzmgrClient, err := newVZMgrServiceClient()
	if err != nil {
		log.WithError(err).Fatal("failed to initialize vizer manager RPC client")
		panic(err)
	}
	server := bridge.NewBridgeGRPCServer(vzmgrClient, nc, sc)
	vzconnpb.RegisterVZConnServiceServer(s.GRPCServer(), server)

	s.Start()
	s.StopOnInterrupt()
}
