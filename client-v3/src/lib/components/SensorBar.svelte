<script lang="ts">
	import type { Sensor } from '$lib/pads.svelte';

	interface Props {
		sensor: Sensor;
		releaseEnabled: boolean; // false in "None" mode: hide the release marker
		onthreshold: (value: number) => void;
		onedit: () => void;
	}

	let { sensor, releaseEnabled, onthreshold, onedit }: Props = $props();

	let trackEl = $state<HTMLDivElement | null>(null);
	let dragging = $state(false);

	const pct = (v: number) => Math.round(v * 100);

	// Pad-authoritative activation (honors release threshold / release mode).
	const active = $derived(sensor.pressed);
	const showRelease = $derived(releaseEnabled && sensor.releaseThreshold !== sensor.threshold);

	function valueFromPointer(clientY: number) {
		if (!trackEl) return sensor.threshold;
		const rect = trackEl.getBoundingClientRect();
		return Math.min(1, Math.max(0, (rect.bottom - clientY) / rect.height));
	}

	function onPointerDown(e: PointerEvent) {
		e.stopPropagation(); // dragging the threshold must not open the editor
		dragging = true;
		(e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
		onthreshold(valueFromPointer(e.clientY));
	}
	function onPointerMove(e: PointerEvent) {
		if (dragging) onthreshold(valueFromPointer(e.clientY));
	}
	function onPointerUp(e: PointerEvent) {
		dragging = false;
		(e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
	}
</script>

<div class="flex h-full flex-col items-center gap-1 select-none">
	<span class="text-muted-foreground text-sm leading-none tabular-nums">
		{pct(sensor.value)}%
	</span>

	<!-- tap the track (outside the drag handle) to open the editor -->
	<div
		bind:this={trackEl}
		class="bg-muted relative w-full min-h-0 flex-1 cursor-pointer overflow-hidden rounded-md"
		onclick={onedit}
		role="button"
		tabindex="0"
		aria-label="Edit button {sensor.button}"
		onkeydown={(e) => (e.key === 'Enter' || e.key === ' ') && onedit()}
	>
		<!-- value fill -->
		<div
			class="pointer-events-none absolute inset-x-0 bottom-0 transition-[height] duration-75 ease-out {active
				? 'bg-primary shadow-[0_0_18px_2px_var(--color-primary)]'
				: 'bg-secondary-foreground/25'}"
			style="height: {pct(sensor.value)}%"
		></div>

		<!-- release-threshold marker (read-only; edited only in the editor) -->
		{#if showRelease}
			<div
				class="pointer-events-none absolute inset-x-0 h-0.5 bg-amber-400 shadow-[0_0_6px_theme(colors.amber.400)]"
				style="bottom: calc({pct(sensor.releaseThreshold)}% - 1px)"
				title="Release threshold"
			></div>
		{/if}

		<!-- visible threshold line -->
		<div
			class="bg-primary-foreground pointer-events-none absolute inset-x-0 h-0.5 shadow-[0_0_6px_var(--color-primary)]"
			style="bottom: calc({pct(sensor.threshold)}% - 1px)"
		></div>

		<!-- oversized drag handle (touch-friendly), centered on the line -->
		<div
			class="absolute inset-x-0 h-11 cursor-row-resize touch-none"
			style="bottom: calc({pct(sensor.threshold)}% - 22px)"
			onclick={(e) => e.stopPropagation()}
			onpointerdown={onPointerDown}
			onpointermove={onPointerMove}
			onpointerup={onPointerUp}
			onpointercancel={onPointerUp}
			role="slider"
			aria-label="Threshold for button {sensor.button}"
			aria-valuemin={0}
			aria-valuemax={100}
			aria-valuenow={pct(sensor.threshold)}
			tabindex="0"
		></div>
	</div>

	<span class="text-primary text-sm leading-none font-medium tabular-nums">
		{pct(sensor.threshold)}%
	</span>
</div>
