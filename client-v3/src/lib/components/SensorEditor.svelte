<script lang="ts">
	import type { Sensor } from '$lib/pads.svelte';
	import { Input } from '$lib/components/ui/input';
	import { Button } from '$lib/components/ui/button';

	interface Props {
		sensor: Sensor; // optimistic-overlaid values
		index: number; // snapshot sensor index
		releaseEnabled: boolean; // false in "None" mode: hide the release slider
		onthreshold: (value: number) => void; // 0..1
		onrelease: (value: number) => void; // 0..1
		ongain: (byte: number) => void; // 0..255
		onclose: () => void;
	}

	let { sensor, index, releaseEnabled, onthreshold, onrelease, ongain, onclose }: Props = $props();

	// Gain: higher % = more sensitivity = LOWER resistor byte.
	// 100% -> byte 0, 0% -> byte 255 (inverted).
	const clampPct = (p: number) => (p < 0 ? 0 : p > 100 ? 100 : p);
	const pctToByte = (p: number) => Math.round(((100 - clampPct(p)) / 100) * 255);
	const byteToPct = (b: number) => Math.round(((255 - b) / 255) * 100);

	const gainPct = $derived(byteToPct(sensor.resistorValue));
	const thresholdPct = $derived(Math.round(sensor.threshold * 100));
	const releasePct = $derived(Math.round(sensor.releaseThreshold * 100));

	let dialogEl = $state<HTMLDialogElement | null>(null);
	$effect(() => {
		dialogEl?.showModal();
	});

	const num = (e: Event) => Number((e.currentTarget as HTMLInputElement).value);
</script>

<dialog
	bind:this={dialogEl}
	onclose={onclose}
	onclick={(e) => e.target === dialogEl && dialogEl?.close()}
	class="bg-card text-card-foreground m-auto w-[min(92vw,26rem)] rounded-lg border p-6 backdrop:bg-black/50"
>
	<div class="flex flex-col gap-5">
		<header class="flex items-center justify-between">
			<h2 class="text-lg font-semibold">
				Sensor {index} · Button {sensor.button}
			</h2>
			<Button variant="ghost" size="sm" onclick={() => dialogEl?.close()}>Done</Button>
		</header>

		{#snippet row(
			label: string,
			value: number,
			unit: string,
			commit: (v: number) => void,
			max: number
		)}
			<div class="flex flex-col gap-1">
				<span class="text-muted-foreground text-sm">{label}</span>
				<div class="flex items-center gap-3">
					<input
						type="range"
						min="0"
						{max}
						{value}
						oninput={(e) => commit(num(e))}
						aria-label={label}
						class="accent-primary h-2 flex-1 cursor-pointer"
					/>
					<div class="flex items-center gap-1">
						<Input
							type="number"
							min={0}
							{max}
							{value}
							oninput={(e) => commit(num(e))}
							aria-label="{label} value"
							class="w-16 text-right tabular-nums"
						/>
						<span class="text-muted-foreground w-3 text-sm">{unit}</span>
					</div>
				</div>
				<div class="flex gap-1">
					<Button
						variant="outline"
						size="icon-sm"
						class="w-16"
						onclick={() => commit(value - 1)}
						aria-label="Decrease {label}"
					>
						−
					</Button>
					<Button
						variant="outline"
						size="icon-sm"
						class="w-16"
						onclick={() => commit(value + 1)}
						aria-label="Increase {label}"
					>
						+
					</Button>
				</div>
			</div>
		{/snippet}

		{@render row('Gain', gainPct, '%', (p) => ongain(pctToByte(p)), 100)}
		{@render row('Threshold', thresholdPct, '%', (p) => onthreshold(clampPct(p) / 100), 100)}
		{#if releaseEnabled}
			<!-- Release can never exceed the threshold, or hysteresis breaks; cap it. -->
			{@render row(
				'Release threshold',
				releasePct,
				'%',
				(p) => onrelease(Math.min(clampPct(p), thresholdPct) / 100),
				thresholdPct
			)}
		{/if}
	</div>
</dialog>
