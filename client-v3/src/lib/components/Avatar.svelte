<script lang="ts">
	import { pads, type Player } from '$lib/pads.svelte';

	let { player, class: klass = 'size-16' }: { player: Player; class?: string } = $props();

	// Fetching writes state, so it runs in an effect; the derived only reads.
	$effect(() => pads.requestAvatar(player));
	const avatar = $derived(pads.avatar(player));
	const initials = $derived(
		player.name
			.split(/\s+/)
			.map((w) => w[0] ?? '')
			.join('')
			.slice(0, 2)
			.toUpperCase()
	);
</script>

<div
	class="bg-muted text-muted-foreground flex shrink-0 items-center justify-center overflow-hidden rounded-full font-semibold {klass}"
>
	{#if avatar?.url && avatar.mime.startsWith('video/')}
		<video src={avatar.url} autoplay muted loop playsinline class="size-full object-cover"></video>
	{:else if avatar?.url}
		<img src={avatar.url} alt="" class="size-full object-cover" />
	{:else}
		{initials}
	{/if}
</div>
