package esutils_test

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/olivere/elastic/v7"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"pixielabs.ai/pixielabs/src/cloud/shared/esutils"
)

func TestIndexMigrate(t *testing.T) {
	testCases := []struct {
		name               string
		indexName          string
		aliasName          string
		indexJSON          string
		expectErr          bool
		errMsg             string
		createBeforeConfig *struct {
			aliasName string
			indexJSON string
		}
		expectedIndexJSON string
	}{
		{
			name:      "creates a new index when one doesn't exist",
			indexName: "test_create_index-0000",
			aliasName: "test_create_index",
			expectErr: false,
			expectedIndexJSON: `{
				"aliases": {
					"test_create_index": {
						"is_write_index": true
					}
				}
			}`,
			createBeforeConfig: nil,
		},
		{
			name:      "updates mappings to add new mapping field including old field in update body",
			indexName: "test_update_mappings_full-000",
			aliasName: "test_update_mappings_full",
			indexJSON: `
			{
				"mappings": {
					"properties": {
						"new_field": {
							"type": "text"
						},
						"old_field": {
							"type": "keyword"
						}
					}
				}
			}
			`,
			expectedIndexJSON: `
			{
				"mappings": {
					"properties": {
						"new_field": {
							"type": "text"
						},
						"old_field": {
							"type": "keyword"
						}
					}
				},
				"aliases": {
					"test_update_mappings_full": {
						"is_write_index": true
					}
				}
			}
			`,
			expectErr: false,
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				aliasName: "test_update_mappings_full",
				indexJSON: `
				{
					"mappings": {
						"properties": {
							"old_field": {
								"type": "keyword"
							}
						}
					}
				}
				`,
			},
		},
		{
			name:      "updates mappings to add new mapping field",
			indexName: "test_update_mappings-000",
			aliasName: "test_update_mappings",
			indexJSON: `
			{
				"mappings": {
					"properties": {
						"new_field": {
							"type": "text"
						}
					}
				}
			}
			`,
			expectedIndexJSON: `
			{
				"mappings": {
					"properties": {
						"new_field": {
							"type": "text"
						},
						"old_field": {
							"type": "keyword"
						}
					}
				},
				"aliases": {
					"test_update_mappings": {
						"is_write_index": true
					}
				}
			}
			`,
			expectErr: false,
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				aliasName: "test_update_mappings",
				indexJSON: `
				{
					"mappings": {
						"properties": {
							"old_field": {
								"type": "keyword"
							}
						}
					}
				}
				`,
			},
		},
		{
			name:      "updates settings",
			indexName: "test_update_settings-000",
			indexJSON: `
			{
				"settings": {
					"index": {
						"blocks": {
							"write": "true",
							"read": "false"
						}
					}
				}
			}
			`,
			expectedIndexJSON: `
			{
				"settings": {
					"index": {
						"blocks": {
							"write": "true",
							"read": "false"
						}
					}
				}
			}
			`,
			expectErr: false,
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				indexJSON: `
				{
					"settings": {
						"index": {
							"blocks": {
								"write": "false",
								"read": "true"
							}
						}
					}
				}
				`,
			},
		},
		{
			name:      "updates aliases remove alias",
			indexName: "test_remove_alias-000",
			aliasName: "test_remove_alias",
			expectedIndexJSON: `
			{
				"aliases": {
					"test_remove_alias": {
						"is_write_index": true
					}
				}
			}
			`,
			expectErr: false,
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				aliasName: "test_remove_alias",
				indexJSON: `
				{
					"aliases": {
						"random_alias_should_be_deleted": {}
					}
				}
				`,
			},
		},
		{
			name:      "updates aliases add alias",
			indexName: "test_update_alias-000",
			aliasName: "test_update_alias",
			indexJSON: `
			{
				"aliases": {
					"other_alias": {}
				}
			}
			`,
			expectedIndexJSON: `
			{
				"aliases": {
					"test_update_alias": {
						"is_write_index": true
					},
					"other_alias": {}
				}
			}
			`,
			expectErr: false,
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				aliasName: "test_update_alias",
			},
		},
		{
			name:      "changing static settings requires reindex",
			indexName: "test_static_settings_reindex-000",
			aliasName: "test_static_settings_reindex",
			indexJSON: `
			{
				"settings": {
					"index": {
						"number_of_shards": 2
					}
				}
			}
			`,
			expectErr: true,
			errMsg:    "reindexing is currently not supported",
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				aliasName: "test_static_settings_reindex",
				// index.number_of_shards is a static setting that can only be changed at index creation time.
				indexJSON: `
				{
					"settings": {
						"index": {
							"number_of_shards": 1
						}
					}
				}
				`,
			},
		},
		{
			name:      "changing mapping property type requires reindex",
			indexName: "test_mapping_reindex-000",
			aliasName: "test_mapping_reindex",
			indexJSON: `
			{
				"mappings": {
					"properties": {
						"f": {
							"type": "keyword"
						}
					}
				}
			}
			`,
			expectErr: true,
			errMsg:    "reindexing is currently not supported",
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				aliasName: "test_mapping_reindex",
				// index.number_of_shards is a static setting that can only be changed at index creation time.
				indexJSON: `
				{
					"mappings": {
						"properties": {
							"f": {
								"type": "text"
							}
						}
					}
				}
				`,
			},
		},
		{
			name:      "changing nested mapping property type requires reindex",
			indexName: "test_nested_mapping_reindex-000",
			aliasName: "test_nested_mapping_reindex",
			indexJSON: `
			{
				"mappings": {
					"properties": {
						"parent": {
							"properties": {
								"f": {
									"type": "keyword"
								}
							}
						}
					}
				}
			}
			`,
			expectErr: true,
			errMsg:    "reindexing is currently not supported",
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				aliasName: "test_nested_mapping_reindex",
				// index.number_of_shards is a static setting that can only be changed at index creation time.
				indexJSON: `
				{
					"mappings": {
						"properties": {
							"parent": {
								"properties": {
									"f": {
										"type": "text"
									}
								}
							}
						}
					}
				}
				`,
			},
		},
		{
			name:      "non-bool is_write_index produces error on update",
			indexName: "test_non_bool_write-000",
			indexJSON: `
			{
				"aliases": {
					"test": {
						"is_write_index": "true"
					}
				}
			}
			`,
			expectErr: true,
			errMsg:    "is_write_index must be of type bool",
			createBeforeConfig: &struct {
				aliasName string
				indexJSON string
			}{
				indexJSON: `
				{
					"mappings": {
						"properties": {
							"random_prop_to_force_update": {
								"type": "keyword"
							}
						}
					}
				}	
				`,
			},
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {

			if tc.createBeforeConfig != nil {
				i := esutils.NewIndex(elasticClient).Name(tc.indexName)
				if tc.createBeforeConfig.indexJSON != "" {
					i.FromJSONString(tc.createBeforeConfig.indexJSON)
				}
				if tc.createBeforeConfig.aliasName != "" {
					i.AddWriteAlias(tc.createBeforeConfig.aliasName)
				}
				err := i.Migrate(context.Background())
				require.Nil(t, err)
			}

			i := esutils.NewIndex(elasticClient).Name(tc.indexName)
			if tc.indexJSON != "" {
				i.FromJSONString(tc.indexJSON)
			}
			if tc.aliasName != "" {
				i.AddWriteAlias(tc.aliasName)
			}
			err := i.Migrate(context.Background())
			if tc.expectErr {
				require.NotNil(t, err)
				assert.Equal(t, tc.errMsg, err.Error())
				return
			}
			require.Nil(t, err)
			// Only cleanup if the creation was successful.
			defer cleanupIndex(t, tc.indexName)

			respMap, err := elasticClient.IndexGet(tc.indexName).Do(context.Background())
			require.Nil(t, err)
			resp, ok := respMap[tc.indexName]
			require.True(t, ok)

			assertIndicesEqual(t, tc.expectedIndexJSON, resp)
		})
	}
}

func assertIndicesEqual(t *testing.T, expectedJSON string, actualResp *elastic.IndicesGetResponse) {
	var ok bool

	var expectedResp map[string]interface{}
	err := json.Unmarshal([]byte(expectedJSON), &expectedResp)
	require.Nil(t, err)

	var expectedMappings map[string]interface{}
	if expectedResp["mappings"] != nil {
		expectedMappings, ok = expectedResp["mappings"].(map[string]interface{})
		require.True(t, ok)
	} else {
		expectedMappings = make(map[string]interface{})
	}
	var expectedAliases map[string]interface{}
	if expectedResp["aliases"] != nil {
		expectedAliases, ok = expectedResp["aliases"].(map[string]interface{})
		require.True(t, ok)
	} else {
		expectedAliases = make(map[string]interface{})
	}
	var expectedSettings map[string]interface{}
	if expectedResp["settings"] != nil {
		expectedSettings, ok = expectedResp["settings"].(map[string]interface{})
		require.True(t, ok)
	} else {
		expectedSettings = make(map[string]interface{})
	}

	assert.Equalf(t, expectedMappings, actualResp.Mappings, "Mappings not equal")
	assert.Equalf(t, expectedAliases, actualResp.Aliases, "Aliases not equal")

	copyIgnoredSettingsPaths(expectedSettings, actualResp.Settings, "")
	assert.Equalf(t, expectedSettings, actualResp.Settings, "Settings not equal")
}

// settingsIgnorePaths are paths in the index settings that we don't care about for checking index equality.
var settingsIgnorePaths map[string]interface{}

func init() {
	var empty struct{}
	settingsIgnorePaths = map[string]interface{}{
		".index.creation_date":      empty,
		".index.number_of_replicas": empty,
		".index.number_of_shards":   empty,
		".index.provided_name":      empty,
		".index.uuid":               empty,
		".index.version.created":    empty,
	}
}

// some index settings such as index.creation_date aren't useful when determining equality. So this function
// copies the ignored settings from the actual to the expected.
func copyIgnoredSettingsPaths(expected map[string]interface{}, actual map[string]interface{}, currentPath string) {
	for k, v := range actual {
		path := strings.Join([]string{currentPath, k}, ".")
		switch v.(type) {
		case map[string]interface{}:
			if _, ok := expected[k]; !ok || expected[k] == nil {
				expected[k] = make(map[string]interface{})
			}
			copyIgnoredSettingsPaths(expected[k].(map[string]interface{}), v.(map[string]interface{}), path)
		default:
			if _, ok := settingsIgnorePaths[path]; !ok {
				continue
			}
			expected[k] = v
		}
	}
}

func cleanupIndex(t *testing.T, indexName string) {
	resp, err := elasticClient.DeleteIndex(indexName).Do(context.Background())
	require.Nil(t, err)
	require.True(t, resp.Acknowledged)
}
