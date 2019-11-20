package events

// This package provides a small wrapper around segment to provide initialization and a dummy
// writer for development mode. The dummy writer is used if segment credentials are not set.

import (
	"sync"

	log "github.com/sirupsen/logrus"
	"github.com/spf13/pflag"
	"github.com/spf13/viper"
	"gopkg.in/segmentio/analytics-go.v3"
)

func init() {
	pflag.String("segment_write_key", "", "The key to use for segment")
}

var client analytics.Client
var once sync.Once

type dummyClient struct{}

func (d *dummyClient) Enqueue(msg analytics.Message) (err error) {
	if err = msg.Validate(); err != nil {
		return
	}

	log.WithField("msg", msg).Debug("Dummy analytics client, dropping message...")
	return nil
}

func (d *dummyClient) Close() error {
	return nil
}

func getDefaultClient() analytics.Client {
	k := viper.GetString("segment_write_key")
	if len(k) > 0 {
		// Key is specified try to to create segment client.
		return analytics.New(k)
	}
	return &dummyClient{}
}

// SetClient sets the default client used for event tracking.
func SetClient(c analytics.Client) {
	client = c
}

// Client returns the client.
func Client() analytics.Client {
	once.Do(func() {
		// client has already been setup.
		if client != nil {
			return
		}
		SetClient(getDefaultClient())
	})
	return client
}
