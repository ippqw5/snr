# Rendering diagram sources

These SVGs are part of the [illustrated rendering guide](../rendering-flow.md).
Edit the matching DOT file, then regenerate the image.

| Diagram | Question answered | Implementation to check |
| --- | --- | --- |
| `scene-ownership.dot` | Where is data prepared, and who owns it after upload? | Scene, GpuScene, Session, renderWindowed |
| `sample-batch.dot` | How is a batch submitted, waited for, displayed or exported? | renderStep, dispatchImage, traceMain, present/readback |
| `path-bounce.dot` | How does one sample intersect, collect light and continue? | tracePath, traceScene, sampleBsdf, MediumState |

A local Graphviz installation is used only for documentation images, outside project dependencies
and the build. The rendering script uses Python's standard library and downloads nothing.
Run from the repository root:

```sh
python docs/diagrams/render.py
```

The script finds `dot` on PATH and writes SVGs beside their sources. Keep generated SVGs with the
sources so readers do not need Graphviz.
An individual image can also be generated with
`dot -Tsvg docs/diagrams/sample-batch.dot -o docs/diagrams/sample-batch.svg`.

Open generated images to check labels, connectors and branches. Verify that the prose, identifiers
and execution order agree with the implementation. These are logical flows, not timing diagrams.
Dashed arrows mean shared references in diagram 1, another batch in diagram 2, and another bounce
in diagram 3. Full text labels preserve meaning independently of color; color only helps distinguish
CPU management, GPU execution and output.
