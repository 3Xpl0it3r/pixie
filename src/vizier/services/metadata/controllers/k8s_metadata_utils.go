package controllers

//go:generate genny -in=k8s_metadata_utils.tmpl -out k8s_metadata_utils.gen.go gen "Resource=Pod,Service,Namespace,Endpoints,Node"
