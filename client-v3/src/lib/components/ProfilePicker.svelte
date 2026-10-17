<script lang="ts">
	import { pads, MAX_PROFILES, type Profile } from '$lib/pads.svelte';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';

	let newName = $state('');

	const profiles = $derived(pads.playerProfiles);
	const padState = $derived(pads.padState);
	const lastUsed = (p: Profile) => padState?.player === pads.player && padState?.profileId === p.id;
	// Last used on this pad first, then most recently saved.
	const sorted = $derived(
		[...(profiles ?? [])].sort(
			(a, b) => Number(lastUsed(b)) - Number(lastUsed(a)) || (b.savedAt ?? 0) - (a.savedAt ?? 0)
		)
	);
	const full = $derived((profiles?.length ?? 0) >= MAX_PROFILES);
	const sources = $derived(pads.copySources);

	// The store moves on to tuning once the server confirms the load.
	function load(p: Profile) {
		pads.loadProfile(p.id);
	}

	// A player with exactly one profile here gets it loaded right away (once,
	// when arriving from the player step; the header chip comes back here freely).
	$effect(() => {
		if (!pads.autoLoad || profiles === undefined) return;
		pads.autoLoad = false;
		if (profiles.length === 1) load(profiles[0]);
	});

	function create() {
		const name = newName.trim();
		if (!name || full) return;
		pads.saveProfileAs(name); // the store moves on to tuning once the server confirms
		newName = '';
	}

	function rename(p: Profile) {
		const name = window.prompt('Profile name', p.name)?.trim();
		if (name) pads.updateProfile(p.id, name, p.description ?? '');
	}

	function remove(p: Profile) {
		if (window.confirm(`Delete profile "${p.name}"?`)) pads.deleteProfile(p.id);
	}

	const date = (s?: number) => (s ? new Date(s * 1000).toLocaleDateString() : '');
</script>

<div class="flex flex-col gap-5">
	<h1 class="text-2xl font-bold">{pads.player}'s profiles on {pads.device?.name ?? 'this pad'}</h1>

	{#if profiles === undefined}
		<p class="text-muted-foreground">Loading…</p>
	{:else}
		<div class="flex flex-col gap-3">
			{#each sorted as p (p.id)}
				<div class="bg-card ring-foreground/10 flex items-stretch rounded-xl ring-1">
					<button class="flex flex-1 flex-col gap-1 p-4 text-left" onclick={() => load(p)}>
						<span class="flex items-center gap-2 text-lg font-semibold">
							{p.name}
							{#if lastUsed(p)}<span class="text-primary text-xs">last used</span>{/if}
						</span>
						{#if p.description}<span class="text-muted-foreground text-sm">{p.description}</span
							>{/if}
						<span class="text-muted-foreground text-xs">saved {date(p.savedAt)}</span>
					</button>
					<div class="flex flex-col justify-center gap-1 p-2">
						<Button variant="ghost" size="sm" onclick={() => rename(p)}>Rename</Button>
						<Button
							variant="ghost"
							size="sm"
							disabled={full}
							onclick={() => pads.copyProfile(p, `${p.name} copy`)}>Duplicate</Button
						>
						<Button variant="ghost" size="sm" class="text-destructive" onclick={() => remove(p)}
							>Delete</Button
						>
					</div>
				</div>
			{/each}
		</div>

		<div class="bg-card ring-foreground/10 flex flex-col gap-3 rounded-xl p-4 ring-1">
			<span class="font-semibold">
				New profile <span class="text-muted-foreground font-normal"
					>({profiles.length}/{MAX_PROFILES})</span
				>
			</span>
			{#if full}
				<p class="text-muted-foreground text-sm">
					This pad already has {MAX_PROFILES} profiles. Delete one to make room.
				</p>
			{:else}
				<form
					class="flex gap-2"
					onsubmit={(e) => {
						e.preventDefault();
						create();
					}}
				>
					<Input bind:value={newName} placeholder="e.g. Stamina, Tech" class="h-11" />
					<Button type="submit" class="h-11" disabled={!newName.trim()}
						>Save current settings</Button
					>
				</form>
				{#if sources.length > 0}
					<div class="flex flex-col gap-2">
						<span class="text-muted-foreground text-sm">Or copy one of yours from another pad</span>
						<div class="flex flex-wrap gap-2">
							{#each sources as s (s.server + s.pad + s.profile.id)}
								<Button
									variant="outline"
									onclick={() => pads.copyProfile(s.profile, s.profile.name)}
								>
									{s.profile.name}
									<span class="text-muted-foreground"
										>· {s.pad}{s.server !== pads.serverName ? ` (${s.server})` : ''}</span
									>
								</Button>
							{/each}
						</div>
					</div>
				{/if}
			{/if}
		</div>
	{/if}
</div>
