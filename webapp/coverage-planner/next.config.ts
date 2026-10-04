import type { NextConfig } from "next";

const config: NextConfig = {
  // PGlite (local development only) ships a WASM Postgres and loads its files
  // relative to its own package; bundling it breaks those paths, so Node
  // requires it from node_modules instead.
  serverExternalPackages: ["@electric-sql/pglite"],
  poweredByHeader: false,
  headers: async () => [
    {
      source: "/:path*",
      headers: [
        { key: "X-Content-Type-Options", value: "nosniff" },
        { key: "Referrer-Policy", value: "same-origin" },
        { key: "X-Frame-Options", value: "DENY" },
      ],
    },
  ],
};

export default config;
