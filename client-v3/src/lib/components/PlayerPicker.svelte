<script lang="ts">
	import { pads, type Player } from '$lib/pads.svelte';
	import { Input } from '$lib/components/ui/input';
	import Avatar from './Avatar.svelte';

	let query = $state('');

	const padName = $derived(pads.device?.name ?? '');
	const players = $derived(pads.players);
	const recent = $derived(
		(pads.padState?.recent ?? [])
			.map((n) => players.find((p) => p.name === n))
			.filter((p): p is Player => !!p)
			.slice(0, 6)
	);
	const filtered = $derived(
		players
			.filter((p) => p.name.toLowerCase().includes(query.trim().toLowerCase()))
			.sort((a, b) => a.name.localeCompare(b.name))
	);
	const count = (p: Player) => p.profiles[padName] ?? 0;
</script>

{#snippet tile(p: Player)}
	<button
		class="bg-card ring-foreground/10 hover:ring-primary relative flex flex-col items-center gap-2 rounded-xl p-3 ring-1 transition {count(
			p
		) === 0
			? 'opacity-60'
			: ''}"
		onclick={() => pads.selectPlayer(p.name)}
	>
		<Avatar player={p} />
		<span class="w-full truncate text-center font-medium">{p.name}</span>
		{#if count(p) > 0}
			<span
				class="bg-primary text-primary-foreground absolute top-2 right-2 rounded-full px-2 text-xs font-semibold"
				>{count(p)}</span
			>
		{/if}
	</button>
{/snippet}

<div class="flex flex-col gap-5">
	<h1 class="text-2xl font-bold">Who's playing on {padName || 'this pad'}?</h1>

	<Input bind:value={query} placeholder="Search players" class="h-12 text-lg" />

	{#if recent.length > 0 && !query}
		<div class="flex flex-col gap-2">
			<span class="text-muted-foreground text-sm">Recent on this pad</span>
			<div class="grid grid-cols-3 gap-3 sm:grid-cols-6">
				{#each recent as p (p.name)}
					{@render tile(p)}
				{/each}
			</div>
		</div>
	{/if}

	<div class="flex flex-col gap-2">
		{#if !query}<span class="text-muted-foreground text-sm">All players</span>{/if}
		<div class="grid grid-cols-3 gap-3 sm:grid-cols-5 lg:grid-cols-6">
			{#each filtered as p (p.name)}
				{@render tile(p)}
			{/each}
		</div>
		{#if players.length === 0 && !pads.localProfilesOk}
			<p class="text-muted-foreground">
				The player list is offline (LocalProfiles not reachable). Players will appear as soon as it
				is back.
			</p>
		{:else if players.length === 0}
			<p class="text-muted-foreground">
				No players found. Check the server's <code>--local-profiles-dir</code>.
			</p>
		{:else if filtered.length === 0}
			<p class="text-muted-foreground">No player matches "{query}".</p>
		{/if}
	</div>
</div>
