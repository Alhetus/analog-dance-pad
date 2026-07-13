<script lang="ts">
	import { Button } from '$lib/components/ui/button';
	import { Label } from '$lib/components/ui/label';

	interface Props {
		onapply: (offsetPct: number, overwriteRelease: boolean) => void;
		onclose: () => void;
	}

	let { onapply, onclose }: Props = $props();

	// Each preset / the slider sets every mapped sensor's threshold to its current
	// resting value plus this many percentage points.
	const presets = [
		{ label: 'Sensitive', pct: 5 },
		{ label: 'Medium', pct: 10 },
		{ label: 'Stiff', pct: 20 }
	];

	let sliderPct = $state(10);
	let overwriteRelease = $state(true);

	let dialogEl = $state<HTMLDialogElement | null>(null);
	$effect(() => {
		dialogEl?.showModal();
	});
</script>

<dialog
	bind:this={dialogEl}
	{onclose}
	onclick={(e) => e.target === dialogEl && dialogEl?.close()}
	class="bg-card text-card-foreground m-auto w-[min(92vw,26rem)] rounded-lg border p-6 backdrop:bg-black/50"
>
	<div class="flex flex-col gap-5">
		<header class="flex items-center justify-between">
			<h2 class="text-lg font-semibold">Calibrate sensors</h2>
			<Button variant="ghost" size="sm" onclick={() => dialogEl?.close()}>Done</Button>
		</header>

		<p class="text-muted-foreground text-sm">
			Sets each mapped sensor's threshold relative to its current resting value — stand off the pad
			before calibrating.
		</p>

		<div class="flex flex-col gap-2">
			<span class="text-muted-foreground text-sm">Presets</span>
			<div class="flex gap-2">
				{#each presets as p (p.pct)}
					<Button variant="outline" class="flex-1" onclick={() => onapply(p.pct, overwriteRelease)}>
						{p.label}
					</Button>
				{/each}
			</div>
		</div>

		<div class="flex flex-col gap-1">
			<div class="flex items-center justify-between">
				<span class="text-muted-foreground text-sm">Custom sensitivity</span>
				<span class="text-sm tabular-nums">+{sliderPct}%</span>
			</div>
			<input
				type="range"
				min="1"
				max="50"
				value={sliderPct}
				oninput={(e) => (sliderPct = Number(e.currentTarget.value))}
				onchange={() => onapply(sliderPct, overwriteRelease)}
				aria-label="Custom sensitivity"
				class="accent-primary h-2 w-full cursor-pointer"
			/>
		</div>

		<Label class="flex items-center gap-2">
			<input type="checkbox" bind:checked={overwriteRelease} class="accent-primary size-4" />
			<span>Overwrite release thresholds</span>
		</Label>
	</div>
</dialog>
