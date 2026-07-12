<script lang="ts">
	import './layout.css';
	import { onMount } from 'svelte';
	import favicon from '$lib/assets/favicon.svg';
	import { pads } from '$lib/pads.svelte';

	let { children } = $props();

	// Own the connection lifecycle here so it survives navigation between routes
	// (e.g. to /profiles); the pages just read the shared `pads` singleton.
	onMount(() => {
		pads.start();
		return () => pads.stop();
	});
</script>

<svelte:head><link rel="icon" href={favicon} /></svelte:head>
{@render children()}
