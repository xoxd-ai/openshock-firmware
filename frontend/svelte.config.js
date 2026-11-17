import adapter from '@sveltejs/adapter-static';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

const versionName = process.env.SOFTPROXY_FRONTEND_VERSION_NAME || process.env.SOURCE_DATE_EPOCH;

/** @type {import('@sveltejs/kit').Config} */
const config = {
  // Consult https://svelte.dev/docs/kit/integrations
  // for more information about preprocessors
  preprocess: vitePreprocess(),

  kit: {
    // adapter-auto only supports some environments, see https://svelte.dev/docs/kit/adapter-auto for a list.
    // If your environment is not supported, or you settled on a specific environment, switch out the adapter.
    // See https://svelte.dev/docs/kit/adapters for more information about adapters.
    adapter: adapter(),
    output: {
      bundleStrategy: 'inline',
    },
    // Softproxy fork (TIN-7545): SvelteKit names the build with Date.now() by default, and the name also seeds the
    // __sveltekit_<hash> global. SOFTPROXY_FRONTEND_VERSION_NAME or else SOURCE_DATE_EPOCH, when set, names it
    // instead, so a rebuild is byte-identical. Unset, as in CI, the default stays.
    ...(versionName ? { version: { name: versionName } } : {}),
  },
};

export default config;
