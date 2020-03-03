package esutils

import (
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"io/ioutil"
	"net/http"
	"strings"

	"github.com/olivere/elastic/v7"
)

// Config describes the underlying config for elastic.
type Config struct {
	URL        []string `json:"url"`
	User       string   `json:"user"`
	Passwd     string   `json:"passwd"`
	CaCertFile string   `json:"ca_cert_file"`
}

func getESHTTPSClient(config *Config) (*http.Client, error) {
	caCert, err := ioutil.ReadFile(config.CaCertFile)
	if err != nil {
		return nil, err
	}
	caCertPool := x509.NewCertPool()
	ok := caCertPool.AppendCertsFromPEM(caCert)
	if !ok {
		return nil, fmt.Errorf("failed to append caCert to pool")
	}
	tlsConfig := &tls.Config{
		RootCAs: caCertPool,
	}
	tlsConfig.BuildNameToCertificate()
	transport := &http.Transport{
		TLSClientConfig: tlsConfig,
	}
	httpClient := &http.Client{
		Transport: transport,
	}
	return httpClient, nil
}

func setupHTTPS(config *Config) ([]elastic.ClientOptionFunc, error) {
	var opts []elastic.ClientOptionFunc
	httpClient, err := getESHTTPSClient(config)
	if err != nil {
		return opts, err
	}
	opts = append(opts,
		elastic.SetHttpClient(httpClient),
		elastic.SetBasicAuth(config.User, config.Passwd))
	return opts, nil
}

// NewEsClient creates an elastic search client from the config.
func NewEsClient(config *Config) (*elastic.Client, error) {
	var opts []elastic.ClientOptionFunc

	// Sniffer should look for HTTPS URLs if at-least-one initial URL is HTTPS
	for _, url := range config.URL {
		if strings.HasPrefix(url, "https:") {
			httpsOpts, err := setupHTTPS(config)
			if err != nil {
				return nil, err
			}
			opts = append(opts, httpsOpts...)
			break
		}
	}

	opts = append(opts, elastic.SetURL(config.URL...), elastic.SetSniff(false))

	return elastic.NewClient(opts...)
}
