<script lang="ts">
	import { pads, type Profile } from '$lib/pads.svelte';
	import { Button } from '$lib/components/ui/button';
	import { Badge } from '$lib/components/ui/badge';
	import { Input } from '$lib/components/ui/input';

	let { profile, loadable }: { profile: Profile; loadable: boolean } = $props();

	// Local editable copies, seeded once from the profile so typing doesn't fight
	// the live list. This component is keyed by profile id, so a new profile
	// remounts and re-seeds. Capturing only the initial value is intentional.
	// svelte-ignore state_referenced_locally
	let name = $state(profile.name);
	// svelte-ignore state_referenced_locally
	let description = $state(profile.description ?? '');

	const pct = (v: number) => `${Math.round(v * 100)}%`;

	function save() {
		if (name.trim()) pads.updateProfile(profile.id, name.trim(), description);
	}

	function remove() {
		if (window.confirm(`Delete profile "${profile.name}"?`)) pads.deleteProfile(profile.id);
	}
</script>

<div class="border-border bg-card flex flex-col gap-3 rounded-lg border p-4">
	<div class="flex flex-wrap items-center gap-2">
		<Input class="max-w-xs" bind:value={name} placeholder="Profile name" />
		<Badge variant="outline">{profile.sensorCount} sensors</Badge>
		<div class="ml-auto flex gap-2">
			<Button size="sm" onclick={save}>Save</Button>
			<Button variant="outline" size="sm" disabled={!loadable} onclick={() => pads.loadProfile(profile.id)}
				>Load to pad</Button
			>
			<Button
				variant="outline"
				size="sm"
				disabled={!loadable}
				onclick={() => pads.overwriteProfile(profile.id)}>Update from pad</Button
			>
			<Button variant="destructive" size="sm" onclick={remove}>Delete</Button>
		</div>
	</div>

	<Input bind:value={description} placeholder="Description (optional)" />

	<div class="text-muted-foreground flex flex-wrap gap-x-4 gap-y-1 text-xs">
		{#each profile.sensors as s, i (i)}
			<span>#{i + 1}: {pct(s.threshold)}/{pct(s.releaseThreshold)}</span>
		{/each}
	</div>
</div>
