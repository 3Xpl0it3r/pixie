package esutils_test

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/stretchr/testify/assert"

	"github.com/stretchr/testify/require"

	"pixielabs.ai/pixielabs/src/cloud/shared/esutils"
)

func cleanupTemplate(t *testing.T, templateName string) {
	resp, err := elasticClient.IndexDeleteTemplate(templateName).Do(context.Background())
	require.Nil(t, err)
	require.True(t, resp.Acknowledged)
}

func TestIndexTemplateMigrate(t *testing.T) {
	testCases := []struct {
		name               string
		policyName         string
		templateName       string
		aliasName          string
		expectErr          bool
		createBeforeConfig *struct {
			policyName string
			aliasName  string
		}
	}{
		{
			name:               "creates a new template when one doesn't exist",
			policyName:         "test_index_templ_policy",
			templateName:       "test_index_template",
			aliasName:          "test_index",
			expectErr:          false,
			createBeforeConfig: nil,
		},
		{
			name:         "updates a template if the policyName is different",
			policyName:   "test_index_templ_update_policy",
			templateName: "test_index_templ_update",
			aliasName:    "test_index_update",
			expectErr:    false,
			createBeforeConfig: &struct {
				policyName string
				aliasName  string
			}{
				policyName: "test_index_templ_update_policy_old",
				aliasName:  "test_index_update",
			},
		},
		{
			name:         "updates a template if the aliasName is different",
			policyName:   "test_index_templ_update_policy",
			templateName: "test_index_templ_update",
			aliasName:    "test_index_update",
			expectErr:    false,
			createBeforeConfig: &struct {
				policyName string
				aliasName  string
			}{
				policyName: "test_index_templ_update_policy",
				aliasName:  "test_index_update_old",
			},
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {

			if tc.createBeforeConfig != nil {
				err := esutils.NewIndexTemplate(elasticClient, tc.templateName).
					AssociateRolloverPolicy(tc.createBeforeConfig.policyName, tc.createBeforeConfig.aliasName).
					Migrate(context.Background())
				require.Nil(t, err)
			}

			expectedTemplate := esutils.NewIndexTemplate(elasticClient, tc.templateName).
				AssociateRolloverPolicy(tc.policyName, tc.aliasName)
			err := expectedTemplate.Migrate(context.Background())
			if tc.expectErr {
				require.NotNil(t, err)
				return
			}
			require.Nil(t, err)
			// Only cleanup if the creation was successful.
			defer cleanupTemplate(t, tc.templateName)

			respMap, err := elasticClient.IndexGetTemplate(tc.templateName).Do(context.Background())
			require.Nil(t, err)
			resp, ok := respMap[tc.templateName]
			require.True(t, ok)

			respStr, err := json.Marshal(resp)
			require.Nil(t, err)

			actualTemplate := esutils.NewIndexTemplate(elasticClient, tc.templateName).FromJSONString(string(respStr))

			assert.Equal(t, expectedTemplate, actualTemplate)
		})
	}
}
