// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

// Host-agnostic deployment.
// SITE_BASE   subpath the site is served from (project Pages on GitHub/Codeberg/GitLab
//             serve at "/<repo>/"; a custom domain serves at "/"). Must start and end with "/".
// SITE_URL    absolute origin used for canonical URLs and the sitemap.
const SITE_BASE = process.env.SITE_BASE ?? '/cdc-badge-os/';
const SITE_URL = process.env.SITE_URL ?? 'https://krim.codeberg.page';

// Prepend the deployment base to internal root-absolute links in Markdown content.
// Authors write bare links like "/guide/foo/"; this rewrites them to work under any
// SITE_BASE (subpath project Pages or a custom-domain root). Idempotent: links that
// already start with the base, protocol-relative links ("//") and external links are
// left untouched. Sibling assets such as /flasher/ and /api/ are prefixed too, since
// they are deployed under the same base.
function rehypeBaseLinks() {
  const prefix = SITE_BASE.replace(/\/$/, ''); // '' when SITE_BASE is '/'
  const walk = (node) => {
    if (
      node.type === 'element' &&
      node.tagName === 'a' &&
      node.properties &&
      typeof node.properties.href === 'string'
    ) {
      const href = node.properties.href;
      if (href.startsWith('/') && !href.startsWith('//')) {
        if (!prefix || !(href === prefix || href.startsWith(prefix + '/'))) {
          node.properties.href = prefix + href;
        }
      }
    }
    if (Array.isArray(node.children)) node.children.forEach(walk);
  };
  return (tree) => walk(tree);
}

export default defineConfig({
  site: SITE_URL,
  base: SITE_BASE,
  trailingSlash: 'always',
  markdown: {
    rehypePlugins: [rehypeBaseLinks],
  },
  integrations: [
    starlight({
      title: 'CDC Badge OS',
      description:
        'Firmware for the CDC Badge v1.0 hardware security key: FIDO2/WebAuthn, GPG/SSH, TOTP, a password vault and a WASM plugin runtime on an ESP32-S3 with a TROPIC01 secure element.',
      logo: {
        light: './src/assets/logo-light.svg',
        dark: './src/assets/logo-dark.svg',
        replacesTitle: false,
      },
      favicon: '/favicon.svg',
      customCss: ['./src/styles/custom.css'],
      editLink: {
        baseUrl: 'https://codeberg.org/Krim/cdc-badge-os/_edit/release/website/',
      },
      social: [
        { icon: 'gitlab', label: 'Codeberg', href: 'https://codeberg.org/Krim/cdc-badge-os' },
        { icon: 'github', label: 'Plugin SDK', href: 'https://github.com/krim404/cdc-badge-plugins' },
      ],
      lastUpdated: true,
      pagination: true,
      sidebar: [
        {
          label: 'Getting Started',
          items: [
            { label: 'Introduction', items: [{ autogenerate: { directory: 'start' } }] },
            { label: 'User Guide', items: [{ autogenerate: { directory: 'guide' } }] },
            { label: 'Security & Background', items: [{ autogenerate: { directory: 'security' } }] },
          ],
        },
        {
          label: 'Intermediate',
          items: [{ autogenerate: { directory: 'power' } }],
        },
        {
          label: 'Developer',
          items: [{ autogenerate: { directory: 'dev' } }],
        },
      ],

    }),
  ],
});
