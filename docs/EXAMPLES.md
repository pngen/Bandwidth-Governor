# Examples

`bg_examples <scenario>` runs a self-contained scenario against the runtime and
prints the resulting resource and flow state. Scenarios:

| scenario | demonstrates |
|---|---|
| `simple` | single-link arbitration |
| `fairness` | weighted tenant fairness |
| `priority` | priority vs fairness |
| `deadline` | deadline-sensitive flow |
| `minguarantee` | minimum-guarantee reservation |
| `maxcap` | maximum-rate cap |
| `multilink` | shared bottleneck multi-link path |
| `independent` | disjoint path independence |
| `throttle` | throttling / backpressure |
| `persistence` | save + recover |
| `cuda` | real host-to-device transfer governance (if CUDA) |

```bash
bg_examples fairness
bg_examples multilink
bg_examples cuda
```

`bg demo <scenario>` provides the same scenarios through the CLI.
