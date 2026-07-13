# sv

Everything you need to build a Svelte project, powered by [`sv`](https://github.com/sveltejs/cli).

## Creating a project

If you're seeing this, you've probably already done this step. Congrats!

```sh
# create a new project
npx sv create my-app
```

To recreate this project with the same configuration:

```sh
# recreate this project
npx sv@0.16.3 create --template minimal --types ts --add eslint prettier tailwindcss="plugins:none" --install npm ./client-v3
```

## Developing

Once you've created a project and installed dependencies with `npm install` (or `pnpm install` or `yarn`), start a development server:

```sh
npm run dev

# or start the server and open the app in a new browser tab
npm run dev -- --open
```

## Configuring servers

The list of ADP Server WebSocket endpoints the app connects to is baked into the
build from [`src/lib/servers.config.json`](src/lib/servers.config.json). There is
no in-app editor — the list is fixed per build.

```json
{
	"servers": ["ws://127.0.0.1:8008"]
}
```

- Each entry is a full WebSocket URL (`ws://host:port` or `wss://host:port`).
  The ADP Server defaults to `ws://127.0.0.1:8008`.
- List multiple endpoints to connect to several servers at once; every device
  found across all of them appears in the pad dropdown, and each endpoint gets a
  connection-status badge on the main page.
- Edit the file and rebuild (or restart `npm run dev`) for changes to take
  effect. The selected pad is remembered in `localStorage`, but the server list
  itself is not — it always comes from this file.

## Building

To create a production version of your app:

```sh
npm run build
```

You can preview the production build with `npm run preview`.

> To deploy your app, you may need to install an [adapter](https://svelte.dev/docs/kit/adapters) for your target environment.
