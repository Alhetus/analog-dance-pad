<script lang="ts">
	import { goto } from '$app/navigation';
	import { pads } from '$lib/pads.svelte';
	import { Button } from '$lib/components/ui/button';
	import ProfileRow from '$lib/components/ProfileRow.svelte';

	const profiles = $derived(pads.profiles);
	const activeCount = $derived(pads.activeSensorCount);
</script>

<div class="mx-auto flex min-h-[100dvh] max-w-4xl flex-col gap-6 p-4 sm:p-6">
	<header class="flex flex-wrap items-center gap-3">
		<h1 class="text-primary mr-auto text-xl font-bold tracking-tight">Profiles</h1>
		<Button variant="outline" size="sm" onclick={() => goto('/')}>← Back</Button>
	</header>

	{#if profiles.length === 0}
		<p class="text-muted-foreground">No profiles yet. Tune a pad and save one from the main screen.</p>
	{/if}

	<div class="flex flex-col gap-4">
		{#each profiles as p (p.id)}
			<ProfileRow profile={p} loadable={activeCount !== null && p.sensorCount === activeCount} />
		{/each}
	</div>
</div>
