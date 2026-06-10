package main

import (
	_ "embed"
	"embed"
	"io/fs"
)

//go:embed frontend
var frontendEmbed embed.FS

var frontendFS, _ = fs.Sub(frontendEmbed, "frontend")

// openapiYAML is the served-as-is API spec. Hand-written; consumed by
// the self-hosted Swagger UI at /api-docs/ and by external tooling
// (codegen, Postman, …) at /api/openapi.yaml.
//
//go:embed openapi.yaml
var openapiYAML []byte
