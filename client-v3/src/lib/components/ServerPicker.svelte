<script lang="ts">
	import { pads } from '$lib/pads.svelte';
</script>

<div class="flex flex-col gap-6">
	<h1 class="text-2xl font-bold">Which machine?</h1>
	<div class="grid grid-cols-1 gap-4 sm:grid-cols-3">
		{#each pads.conns as c (c.endpoint)}
			<button
				class="bg-card ring-foreground/10 hover:ring-primary flex min-h-32 flex-col items-center justify-center gap-2 rounded-xl ring-1 transition"
				onclick={() => pads.selectServer(c.endpoint)}
			>
				<span class="text-3xl font-bold tracking-wide">{c.name}</span>
				<span class="text-muted-foreground flex items-center gap-1.5 text-sm">
					<span
						class="size-2 rounded-full {c.status === 'open'
							? 'bg-green-500'
							: c.status === 'connecting'
								? 'bg-yellow-500'
								: 'bg-red-500'}"
					></span>
					{c.status === 'open'
						? `${c.deviceList.length} ${c.deviceList.length === 1 ? 'pad' : 'pads'}`
						: c.status}
				</span>
			</button>
		{/each}
	</div>
</div>
