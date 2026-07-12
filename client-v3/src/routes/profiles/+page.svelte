<script lang="ts">
	import { goto } from '$app/navigation';
	import { pads } from '$lib/pads.svelte';
	import { Button } from '$lib/components/ui/button';
	import { Badge } from '$lib/components/ui/badge';
	import { Input } from '$lib/components/ui/input';

	const profiles = $derived(pads.profiles);
	const activeCount = $derived(pads.activeSensorCount);

	// Local editable copies keyed by id, so typing doesn't fight the live list.
	let edits = $state<Record<string, { name: string; description: string }>>({});

	function editFor(p: { id: string; name: string; description?: string }) {
		return (edits[p.id] ??= { name: p.name, description: p.description ?? '' });
	}

	function save(id: string) {
		const e = edits[id];
		if (e && e.name.trim()) pads.updateProfile(id, e.name.trim(), e.description);
	}

	function remove(id: string, name: string) {
		if (window.confirm(`Delete profile "${name}"?`)) pads.deleteProfile(id);
	}

	const pct = (v: number) => `${Math.round(v * 100)}%`;
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
			{@const e = editFor(p)}
			{@const loadable = activeCount !== null && p.sensorCount === activeCount}
			<div class="border-border bg-card flex flex-col gap-3 rounded-lg border p-4">
				<div class="flex flex-wrap items-center gap-2">
					<Input class="max-w-xs" bind:value={e.name} placeholder="Profile name" />
					<Badge variant="outline">{p.sensorCount} sensors</Badge>
					<div class="ml-auto flex gap-2">
						<Button size="sm" onclick={() => save(p.id)}>Save</Button>
						<Button
							variant="outline"
							size="sm"
							disabled={!loadable}
							onclick={() => pads.loadProfile(p.id)}>Load to pad</Button
						>
						<Button
							variant="outline"
							size="sm"
							disabled={!loadable}
							onclick={() => pads.overwriteProfile(p.id)}>Update from pad</Button
						>
						<Button variant="destructive" size="sm" onclick={() => remove(p.id, p.name)}>Delete</Button
						>
					</div>
				</div>

				<Input bind:value={e.description} placeholder="Description (optional)" />

				<div class="text-muted-foreground flex flex-wrap gap-x-4 gap-y-1 text-xs">
					{#each p.sensors as s, i (i)}
						<span>#{i + 1}: {pct(s.threshold)}/{pct(s.releaseThreshold)}</span>
					{/each}
				</div>
			</div>
		{/each}
	</div>
</div>
