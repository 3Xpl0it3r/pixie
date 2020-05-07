package autocomplete

import (
	"context"
	"encoding/json"
	"strings"

	"github.com/olivere/elastic/v7"
	"github.com/sahilm/fuzzy"
	uuid "github.com/satori/go.uuid"

	"pixielabs.ai/pixielabs/src/cloud/cloudapipb"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/script"
)

// ElasticSuggester provides suggestions based on the given index in Elastic.
type ElasticSuggester struct {
	client          *elastic.Client
	mdIndexName     string
	scriptIndexName string
	// This is temporary, and will be removed once we start indexing scripts.
	br *script.BundleManager
}

// EsMDEntity is information about metadata entities, stored in Elastic.
// This is copied from the indexer. When the indexer is checked in, we should consider
// putting this in a shared directory.
type EsMDEntity struct {
	OrgID string `json:"orgID"`
	UID   string `json:"uid"`
	Name  string `json:"name"`
	NS    string `json:"ns"`
	Kind  string `json:"kind"`

	TimeStartedNS int64 `json:"timeStartedNS"`
	TimeStoppedNS int64 `json:"timeStoppedNS"`

	RelatedEntityNames []string `json:"relatedEntityNames"`

	ResourceVersion string `json:"resourceVersion"`
	Test            string `json:"test"`
}

var protoToElasticLabelMap = map[cloudapipb.AutocompleteEntityKind]string{
	cloudapipb.AEK_SVC:       "service",
	cloudapipb.AEK_POD:       "pod",
	cloudapipb.AEK_SCRIPT:    "script",
	cloudapipb.AEK_NAMESPACE: "script",
}

var elasticLabelToProtoMap = map[string]cloudapipb.AutocompleteEntityKind{
	"service":   cloudapipb.AEK_SVC,
	"pod":       cloudapipb.AEK_POD,
	"script":    cloudapipb.AEK_SCRIPT,
	"namespace": cloudapipb.AEK_NAMESPACE,
}

// NewElasticSuggester creates a suggester based on an elastic index.
func NewElasticSuggester(client *elastic.Client, mdIndex string, scriptIndex string, br *script.BundleManager) *ElasticSuggester {
	return &ElasticSuggester{client, mdIndex, scriptIndex, br}
}

// SuggestionRequest is a request for autocomplete suggestions.
type SuggestionRequest struct {
	OrgID        uuid.UUID
	Input        string
	AllowedKinds []cloudapipb.AutocompleteEntityKind
	AllowedArgs  []cloudapipb.AutocompleteEntityKind
}

// SuggestionResult contains results for an autocomplete request.
type SuggestionResult struct {
	Suggestions []*Suggestion
	ExactMatch  bool
}

// GetSuggestions get suggestions for the given input using Elastic.
func (e *ElasticSuggester) GetSuggestions(reqs []*SuggestionRequest) ([]*SuggestionResult, error) {
	resps := make([]*SuggestionResult, len(reqs))

	if len(reqs) == 0 {
		return resps, nil
	}

	ms := e.client.MultiSearch()

	for _, r := range reqs {
		ms.Add(elastic.NewSearchRequest().
			Query(e.getQueryForRequest(r.OrgID, r.Input, r.AllowedKinds, r.AllowedArgs)))
	}

	resp, err := ms.Do(context.Background())

	if err != nil {
		return nil, err
	}

	// Parse scripts to prepare for matching. This is temporary until we have script indexing.
	scripts := []string{}
	scriptArgMap := make(map[string][]cloudapipb.AutocompleteEntityKind)
	scriptArgNames := make(map[string][]string)
	if e.br != nil {
		for _, s := range e.br.GetScripts() {
			scripts = append(scripts, s.ScriptName)
			scriptArgMap[s.ScriptName] = make([]cloudapipb.AutocompleteEntityKind, 0)
			for _, a := range s.ComputedArgs() {
				aKind := cloudapipb.AEK_UNKNOWN
				// The args aren't typed yet, so we assume the type from the name.
				if strings.Index(a.Name, "pod") != -1 {
					aKind = cloudapipb.AEK_POD
				}
				if strings.Index(a.Name, "svc") != -1 || strings.Index(a.Name, "service") != -1 {
					aKind = cloudapipb.AEK_SVC
				}

				if aKind != cloudapipb.AEK_UNKNOWN {
					scriptArgMap[s.ScriptName] = append(scriptArgMap[s.ScriptName], aKind)
					scriptArgNames[s.ScriptName] = append(scriptArgNames[s.ScriptName], a.Name)
				}
			}
		}
	}

	for i, r := range resp.Responses {
		// This is temporary until we index scripts in Elastic.
		scriptResults := make([]*Suggestion, 0)
		if e.br != nil {
			for _, t := range reqs[i].AllowedKinds {
				if t == cloudapipb.AEK_SCRIPT { // Script is an allowed type for this tabstop, so we should find matching scripts.
					matches := fuzzy.Find(reqs[i].Input, scripts)
					for _, m := range matches {
						script := e.br.MustGetScript(m.Str)
						scriptArgs := scriptArgMap[m.Str]
						scriptNames := scriptArgNames[m.Str]
						valid := true
						for _, r := range reqs[i].AllowedArgs { // Check that the script takes the allowed args.
							found := false
							for _, arg := range scriptArgs {
								if arg == r {
									found = true
									break
								}
							}
							if !found {
								valid = false
								break
							}
						}
						if valid {
							scriptResults = append(scriptResults, &Suggestion{
								Name:     m.Str,
								Kind:     cloudapipb.AEK_SCRIPT,
								Desc:     script.LongDoc,
								ArgNames: scriptNames,
								ArgKinds: scriptArgs,
							})
						}
					}
					break
				}
			}
		}
		exactMatch := len(scriptResults) > 0 && scriptResults[0].Name == reqs[i].Input

		// Convert elastic entity into a suggestion object.
		results := make([]*Suggestion, 0)
		for _, h := range r.Hits.Hits {
			res := &EsMDEntity{}
			err = json.Unmarshal(h.Source, res)
			if err != nil {
				return nil, err
			}
			results = append(results, &Suggestion{
				Name:  res.NS + "/" + res.Name,
				Score: float64(*h.Score),
				Kind:  elasticLabelToProtoMap[res.Kind],
			})
		}

		exactMatch = exactMatch || len(results) > 0 && results[0].Name == reqs[i].Input

		results = append(scriptResults, results...)

		resps[i] = &SuggestionResult{
			Suggestions: results,
			ExactMatch:  exactMatch,
		}
	}
	return resps, nil
}

func (e *ElasticSuggester) getQueryForRequest(orgID uuid.UUID, input string, allowedKinds []cloudapipb.AutocompleteEntityKind, allowedArgs []cloudapipb.AutocompleteEntityKind) *elastic.BoolQuery {
	q := elastic.NewBoolQuery()

	q.Should(e.getMDEntityQuery(orgID, input, allowedKinds))

	// TODO(michelle): Add script query here once that is ready: q.Should(e.getScriptQuery(orgID, input, allowedArgs))
	return q
}

func (e *ElasticSuggester) getMDEntityQuery(orgID uuid.UUID, input string, allowedKinds []cloudapipb.AutocompleteEntityKind) *elastic.BoolQuery {
	entityQuery := elastic.NewBoolQuery()
	entityQuery.Must(elastic.NewTermQuery("_index", e.mdIndexName))

	// Search by name + namespace.
	splitInput := strings.Split(input, "/") // If contains "/", then everything preceding "/" is a namespace.
	name := input
	if len(splitInput) > 1 {
		entityQuery.Must(elastic.NewTermQuery("ns", splitInput[0]))
		name = splitInput[1]
	}
	if name != "" {
		entityQuery.Must(elastic.NewMatchQuery("name", name))
	}

	// Only search for entities in org.
	entityQuery.Must(elastic.NewTermQuery("orgID", orgID.String()))

	// Only search for allowed kinds.
	kindsQuery := elastic.NewBoolQuery()
	for _, k := range allowedKinds {
		kindsQuery.Should(elastic.NewTermQuery("kind", protoToElasticLabelMap[k]))
	}
	entityQuery.Must(kindsQuery)

	return entityQuery
}

func (e *ElasticSuggester) getScriptQuery(orgID uuid.UUID, input string, allowedArgs []cloudapipb.AutocompleteEntityKind) *elastic.BoolQuery {
	// TODO(michelle): Handle scripts once we get a better idea of what the index looks like.
	scriptQuery := elastic.NewBoolQuery()
	scriptQuery.Must(elastic.NewTermQuery("_index", "scripts"))

	return scriptQuery
}
