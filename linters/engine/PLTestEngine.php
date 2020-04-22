<?php

include 'FileCheckerTestEngine.php';
include 'GazelleCheckerTestEngine.php';
include 'ExpCheckerTestEngine.php';
include 'GoGenerateCheckerTestEngine.php';
include 'PrototoolCheckerTestEngine.php';

final class PLTestEngine extends ArcanistUnitTestEngine {
    private $project_root;
    private $files;

    public function run() {
        $this->project_root = $this->getWorkingCopy()->getProjectRoot();

        $test_results = array();

        $this->files = $this->getPaths();

        $file_checker = new FileCheckerTest($this->project_root, $this->files);
        $test_results = array_merge($test_results, $file_checker->run());

        $gazelle_checker = new GazelleCheckerTest($this->project_root, $this->files);
        $test_results = array_merge($test_results, $gazelle_checker->run());

        $exp_checker = new ExpCheckerTest($this->project_root, $this->files);
        $test_results = array_merge($test_results, $exp_checker->run());

        // TODO(michelle): Determine a more robust check.
        // $go_generate_checker = new GoGenerateCheckerTest($this->project_root, $this->files);
        // $test_results = array_merge($test_results, $go_generate_checker->run());

    	$prototool_checker = new PrototoolCheckerTest($this->project_root, $this->files);
    	$test_results = array_merge($test_results, $prototool_checker->run());

        return $test_results;
    }
}
