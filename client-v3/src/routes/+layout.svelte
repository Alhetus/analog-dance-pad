<script lang="ts">
	import './layout.css';
	import { onMount } from 'svelte';
	import favicon from '$lib/assets/favicon.svg';
	import { pads } from '$lib/pads.svelte';
	import { Button } from '$lib/components/ui/button';

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

{#if pads.notice}
	{@const notice = pads.notice}
	<div
		role="status"
		class="bg-card fixed bottom-6 left-1/2 flex -translate-x-1/2 items-center gap-3 rounded-full px-5 py-3 shadow-lg ring-1 {notice.ok
			? 'ring-foreground/10'
			: 'ring-destructive text-destructive'}"
	>
		<span>{notice.text}</span>
		{#if notice.ok && notice.op === 'loadProfile'}
			<Button
				variant="link"
				size="sm"
				onclick={() => {
					pads.notice = null;
					pads.goto('profile');
				}}>Change</Button
			>
		{/if}
	</div>
{/if}
