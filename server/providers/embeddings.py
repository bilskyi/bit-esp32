"""Local embeddings for fact retrieval.

fastembed runs ONNX Runtime, not PyTorch - no GPU, no GB-scale download, and
it fits the same "free forever, no external account" bar the rest of this
project holds its dependencies to.

The default model, `paraphrase-multilingual-MiniLM-L12-v2`, does not need
query/passage prefixes - fastembed's own registry says so explicitly for
this one. That is not true of every embedding model (multilingual-e5 is
trained asymmetrically and needs one), which is why `Embedder` still has two
methods rather than one: swapping in an asymmetric model later is a change
to this file, not to every caller.
"""

import asyncio

from fastembed import TextEmbedding


class FastEmbedEmbedder:
    def __init__(self, model_name: str, cache_dir: str | None = None) -> None:
        kwargs = {"cache_dir": cache_dir} if cache_dir else {}
        self._model = TextEmbedding(model_name=model_name, **kwargs)

    def _encode(self, texts: list[str]) -> list[list[float]]:
        return [vector.tolist() for vector in self._model.embed(texts)]

    async def embed_documents(self, texts: list[str]) -> list[list[float]]:
        return await asyncio.to_thread(self._encode, texts)

    async def embed_query(self, text: str) -> list[float]:
        vectors = await asyncio.to_thread(self._encode, [text])
        return vectors[0]
