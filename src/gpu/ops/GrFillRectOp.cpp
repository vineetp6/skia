/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/ops/GrFillRectOp.h"

#include "include/core/SkMatrix.h"
#include "include/core/SkRect.h"
#include "src/gpu/GrCaps.h"
#include "src/gpu/GrGeometryProcessor.h"
#include "src/gpu/GrPaint.h"
#include "src/gpu/GrProgramInfo.h"
#include "src/gpu/SkGr.h"
#include "src/gpu/geometry/GrQuad.h"
#include "src/gpu/geometry/GrQuadBuffer.h"
#include "src/gpu/geometry/GrQuadUtils.h"
#include "src/gpu/glsl/GrGLSLColorSpaceXformHelper.h"
#include "src/gpu/glsl/GrGLSLGeometryProcessor.h"
#include "src/gpu/glsl/GrGLSLVarying.h"
#include "src/gpu/ops/GrMeshDrawOp.h"
#include "src/gpu/ops/GrQuadPerEdgeAA.h"
#include "src/gpu/ops/GrSimpleMeshDrawOpHelperWithStencil.h"

namespace {

using VertexSpec = GrQuadPerEdgeAA::VertexSpec;
using ColorType = GrQuadPerEdgeAA::ColorType;

#ifdef SK_DEBUG
static SkString dump_quad_info(int index, const GrQuad* deviceQuad,
                               const GrQuad* localQuad, const SkPMColor4f& color,
                               GrQuadAAFlags aaFlags) {
    GrQuad safeLocal = localQuad ? *localQuad : GrQuad();
    SkString str;
    str.appendf("%d: Color: [%.2f, %.2f, %.2f, %.2f], Edge AA: l%u_t%u_r%u_b%u, \n"
                "  device quad: [(%.2f, %2.f, %.2f), (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f), "
                "(%.2f, %.2f, %.2f)],\n"
                "  local quad: [(%.2f, %2.f, %.2f), (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f), "
                "(%.2f, %.2f, %.2f)]\n",
                index, color.fR, color.fG, color.fB, color.fA,
                (uint32_t) (aaFlags & GrQuadAAFlags::kLeft),
                (uint32_t) (aaFlags & GrQuadAAFlags::kTop),
                (uint32_t) (aaFlags & GrQuadAAFlags::kRight),
                (uint32_t) (aaFlags & GrQuadAAFlags::kBottom),
                deviceQuad->x(0), deviceQuad->y(0), deviceQuad->w(0),
                deviceQuad->x(1), deviceQuad->y(1), deviceQuad->w(1),
                deviceQuad->x(2), deviceQuad->y(2), deviceQuad->w(2),
                deviceQuad->x(3), deviceQuad->y(3), deviceQuad->w(3),
                safeLocal.x(0), safeLocal.y(0), safeLocal.w(0),
                safeLocal.x(1), safeLocal.y(1), safeLocal.w(1),
                safeLocal.x(2), safeLocal.y(2), safeLocal.w(2),
                safeLocal.x(3), safeLocal.y(3), safeLocal.w(3));
    return str;
}
#endif

class FillRectOp final : public GrMeshDrawOp {
private:
    using Helper = GrSimpleMeshDrawOpHelperWithStencil;

public:
    static std::unique_ptr<GrDrawOp> Make(GrRecordingContext* context,
                                          GrPaint&& paint,
                                          GrAAType aaType,
                                          DrawQuad* quad,
                                          const GrUserStencilSettings* stencilSettings,
                                          Helper::InputFlags inputFlags) {
        // Clean up deviations between aaType and edgeAA
        GrQuadUtils::ResolveAAType(aaType, quad->fEdgeFlags, quad->fDevice,
                                   &aaType, &quad->fEdgeFlags);
        return Helper::FactoryHelper<FillRectOp>(context, std::move(paint), aaType, quad,
                                                 stencilSettings, inputFlags);
    }

    // aaType is passed to Helper in the initializer list, so incongruities between aaType and
    // edgeFlags must be resolved prior to calling this constructor.
    FillRectOp(Helper::MakeArgs args, SkPMColor4f paintColor, GrAAType aaType,
               DrawQuad* quad, const GrUserStencilSettings* stencil, Helper::InputFlags inputFlags)
            : INHERITED(ClassID())
            , fHelper(args, aaType, stencil, inputFlags)
            , fQuads(1, !fHelper.isTrivial()) {
        // Set bounds before clipping so we don't have to worry about unioning the bounds of
        // the two potential quads (GrQuad::bounds() is perspective-safe).
        this->setBounds(quad->fDevice.bounds(), HasAABloat(aaType == GrAAType::kCoverage),
                        IsHairline::kNo);

        DrawQuad extra;
        // Only clip when there's anti-aliasing. When non-aa, the GPU clips just fine and there's
        // no inset/outset math that requires w > 0.
        int count = quad->fEdgeFlags != GrQuadAAFlags::kNone ? GrQuadUtils::ClipToW0(quad, &extra)
                                                             : 1;
        if (count == 0) {
            // We can't discard the op at this point, but disable AA flags so it won't go through
            // inset/outset processing
            quad->fEdgeFlags = GrQuadAAFlags::kNone;
            count = 1;
        }

        // Conservatively keep track of the local coordinates; it may be that the paint doesn't
        // need them after analysis is finished. If the paint is known to be solid up front they
        // can be skipped entirely.
        fQuads.append(quad->fDevice, {paintColor, quad->fEdgeFlags},
                      fHelper.isTrivial() ? nullptr : &quad->fLocal);
        if (count > 1) {
            fQuads.append(extra.fDevice, { paintColor, extra.fEdgeFlags },
                          fHelper.isTrivial() ? nullptr : &extra.fLocal);
        }
    }

    const char* name() const override { return "FillRectOp"; }

    void visitProxies(const VisitProxyFunc& func) const override {
        if (fProgramInfo) {
            fProgramInfo->visitProxies(func);
        } else {
            return fHelper.visitProxies(func);
        }
    }

#ifdef SK_DEBUG
    SkString dumpInfo() const override {
        SkString str;
        str.appendf("# draws: %u\n", fQuads.count());
        str.appendf("Device quad type: %u, local quad type: %u\n",
                    (uint32_t) fQuads.deviceQuadType(), (uint32_t) fQuads.localQuadType());
        str += fHelper.dumpInfo();
        int i = 0;
        auto iter = fQuads.iterator();
        while(iter.next()) {
            const ColorAndAA& info = iter.metadata();
            str += dump_quad_info(i, iter.deviceQuad(), iter.localQuad(),
                                  info.fColor, info.fAAFlags);
            i++;
        }
        str += INHERITED::dumpInfo();
        return str;
    }
#endif

    GrProcessorSet::Analysis finalize(
            const GrCaps& caps, const GrAppliedClip* clip, bool hasMixedSampledCoverage,
            GrClampType clampType) override {
        // Initialize aggregate color analysis with the first quad's color (which always exists)
        auto iter = fQuads.metadata();
        SkAssertResult(iter.next());
        GrProcessorAnalysisColor quadColors(iter->fColor);
        // Then combine the colors of any additional quads (e.g. from MakeSet)
        while(iter.next()) {
            quadColors = GrProcessorAnalysisColor::Combine(quadColors, iter->fColor);
            if (quadColors.isUnknown()) {
                // No point in accumulating additional starting colors, combining cannot make it
                // less unknown.
                break;
            }
        }

        // If the AA type is coverage, it will be a single value per pixel; if it's not coverage AA
        // then the coverage is always 1.0, so specify kNone for more optimal blending.
        GrProcessorAnalysisCoverage coverage = fHelper.aaType() == GrAAType::kCoverage ?
                GrProcessorAnalysisCoverage::kSingleChannel :
                GrProcessorAnalysisCoverage::kNone;
        auto result = fHelper.finalizeProcessors(
                caps, clip, hasMixedSampledCoverage, clampType, coverage, &quadColors);
        // If there is a constant color after analysis, that means all of the quads should be set
        // to the same color (even if they started out with different colors).
        iter = fQuads.metadata();
        SkPMColor4f colorOverride;
        if (quadColors.isConstant(&colorOverride)) {
            fColorType = GrQuadPerEdgeAA::MinColorType(colorOverride);
            while(iter.next()) {
                iter->fColor = colorOverride;
            }
        } else {
            // Otherwise compute the color type needed as the max over all quads.
            fColorType = ColorType::kNone;
            while(iter.next()) {
                fColorType = std::max(fColorType, GrQuadPerEdgeAA::MinColorType(iter->fColor));
            }
        }
        // Most SkShaders' FPs multiply their calculated color by the paint color or alpha. We want
        // to use ColorType::kNone to optimize out that multiply. However, if there are no color
        // FPs then were really writing a special shader for white rectangles and not saving any
        // multiples. So in that case use bytes to avoid the extra shader (and possibly work around
        // an ANGLE issue: crbug.com/942565).
        if (fColorType == ColorType::kNone && !result.hasColorFragmentProcessor()) {
            fColorType = ColorType::kByte;
        }

        return result;
    }

    FixedFunctionFlags fixedFunctionFlags() const override {
        // Since the AA type of the whole primitive is kept consistent with the per edge AA flags
        // the helper's fixed function flags are appropriate.
        return fHelper.fixedFunctionFlags();
    }

    DEFINE_OP_CLASS_ID

private:
    friend class ::GrFillRectOp; // for access to addQuad

#if GR_TEST_UTILS
    int numQuads() const final { return fQuads.count(); }
#endif

    VertexSpec vertexSpec() const {
        auto indexBufferOption = GrQuadPerEdgeAA::CalcIndexBufferOption(fHelper.aaType(),
                                                                        fQuads.count());

        return VertexSpec(fQuads.deviceQuadType(), fColorType, fQuads.localQuadType(),
                          fHelper.usesLocalCoords(), GrQuadPerEdgeAA::Domain::kNo,
                          fHelper.aaType(),
                          fHelper.compatibleWithCoverageAsAlpha(), indexBufferOption);
    }

    GrProgramInfo* programInfo() override {
        // This Op implements its own onPrePrepareDraws so this entry point should never be called.
        SkASSERT(0);
        return fProgramInfo;
    }

    void onCreateProgramInfo(const GrCaps* caps,
                             SkArenaAlloc* arena,
                             const GrSurfaceProxyView* outputView,
                             GrAppliedClip&& appliedClip,
                             const GrXferProcessor::DstProxyView& dstProxyView) override {
        const VertexSpec vertexSpec = this->vertexSpec();

        GrGeometryProcessor* gp = GrQuadPerEdgeAA::MakeProcessor(arena, vertexSpec);
        SkASSERT(gp->vertexStride() == vertexSpec.vertexSize());

        fProgramInfo = fHelper.createProgramInfoWithStencil(caps, arena, outputView,
                                                            std::move(appliedClip),
                                                            dstProxyView, gp,
                                                            vertexSpec.primitiveType());
    }

    void onPrePrepareDraws(GrRecordingContext* context,
                           const GrSurfaceProxyView* outputView,
                           GrAppliedClip* clip,
                           const GrXferProcessor::DstProxyView& dstProxyView) override {
        TRACE_EVENT0("skia.gpu", TRACE_FUNC);

        SkASSERT(!fPrePreparedVertices);

        SkArenaAlloc* arena = context->priv().recordTimeAllocator();

        // This is equivalent to a GrOpFlushState::detachAppliedClip
        GrAppliedClip appliedClip = clip ? std::move(*clip) : GrAppliedClip();

        this->createProgramInfo(context->priv().caps(), arena, outputView,
                                std::move(appliedClip), dstProxyView);

        context->priv().recordProgramInfo(fProgramInfo);

        const VertexSpec vertexSpec = this->vertexSpec();

        const int totalNumVertices = fQuads.count() * vertexSpec.verticesPerQuad();
        const size_t totalVertexSizeInBytes = vertexSpec.vertexSize() * totalNumVertices;

        fPrePreparedVertices = arena->makeArrayDefault<char>(totalVertexSizeInBytes);

        this->tessellate(vertexSpec, fPrePreparedVertices);
    }

    void tessellate(const VertexSpec& vertexSpec, char* dst) const {
        static constexpr SkRect kEmptyDomain = SkRect::MakeEmpty();

        GrQuadPerEdgeAA::Tessellator tessellator(vertexSpec, dst);
        auto iter = fQuads.iterator();
        while (iter.next()) {
            // All entries should have local coords, or no entries should have local coords,
            // matching !helper.isTrivial() (which is more conservative than helper.usesLocalCoords)
            SkASSERT(iter.isLocalValid() != fHelper.isTrivial());
            auto info = iter.metadata();
            tessellator.append(iter.deviceQuad(), iter.localQuad(),
                               info.fColor, kEmptyDomain, info.fAAFlags);
        }
    }

    void onPrepareDraws(Target* target) override {
        TRACE_EVENT0("skia.gpu", TRACE_FUNC);

        const VertexSpec vertexSpec = this->vertexSpec();

        // Make sure that if the op thought it was a solid color, the vertex spec does not use
        // local coords.
        SkASSERT(!fHelper.isTrivial() || !fHelper.usesLocalCoords());

        sk_sp<const GrBuffer> vertexBuffer;
        int vertexOffsetInBuffer = 0;

        const int totalNumVertices = fQuads.count() * vertexSpec.verticesPerQuad();

        // Fill the allocated vertex data
        void* vdata = target->makeVertexSpace(vertexSpec.vertexSize(), totalNumVertices,
                                              &vertexBuffer, &vertexOffsetInBuffer);
        if (!vdata) {
            SkDebugf("Could not allocate vertices\n");
            return;
        }

        if (fPrePreparedVertices) {
            const size_t totalVertexSizeInBytes = vertexSpec.vertexSize() * totalNumVertices;

            memcpy(vdata, fPrePreparedVertices, totalVertexSizeInBytes);
        } else {
            this->tessellate(vertexSpec, (char*) vdata);
        }

        sk_sp<const GrBuffer> indexBuffer;
        if (vertexSpec.needsIndexBuffer()) {
            indexBuffer = GrQuadPerEdgeAA::GetIndexBuffer(target, vertexSpec.indexBufferOption());
            if (!indexBuffer) {
                SkDebugf("Could not allocate indices\n");
                return;
            }
        }

        // Configure the mesh for the vertex data
        fMesh = target->allocMeshes(1);
        GrQuadPerEdgeAA::ConfigureMesh(target->caps(), fMesh, vertexSpec, 0, fQuads.count(),
                                       totalNumVertices, std::move(vertexBuffer),
                                       std::move(indexBuffer), vertexOffsetInBuffer);
    }

    void onExecute(GrOpFlushState* flushState, const SkRect& chainBounds) override {
        if (!fMesh) {
            return;
        }

        if (!fProgramInfo) {
            this->createProgramInfo(flushState);
        }

        flushState->bindPipeline(*fProgramInfo, chainBounds);
        flushState->opsRenderPass()->drawMeshes(*fProgramInfo, fMesh, 1);
    }

    CombineResult onCombineIfPossible(GrOp* t, GrRecordingContext::Arenas*,
                                      const GrCaps& caps) override {
        TRACE_EVENT0("skia.gpu", TRACE_FUNC);
        const auto* that = t->cast<FillRectOp>();

        bool upgradeToCoverageAAOnMerge = false;
        if (fHelper.aaType() != that->fHelper.aaType()) {
            if (!CanUpgradeAAOnMerge(fHelper.aaType(), that->fHelper.aaType())) {
                return CombineResult::kCannotCombine;
            }
            upgradeToCoverageAAOnMerge = true;
        }

        if (CombinedQuadCountWillOverflow(fHelper.aaType(), upgradeToCoverageAAOnMerge,
                                          fQuads.count() + that->fQuads.count())) {
            return CombineResult::kCannotCombine;
        }

        // Unlike most users of the draw op helper, this op can merge none-aa and coverage-aa draw
        // ops together, so pass true as the last argument.
        if (!fHelper.isCompatible(that->fHelper, caps, this->bounds(), that->bounds(), true)) {
            return CombineResult::kCannotCombine;
        }

        // If the paints were compatible, the trivial/solid-color state should be the same
        SkASSERT(fHelper.isTrivial() == that->fHelper.isTrivial());

        // If the processor sets are compatible, the two ops are always compatible; it just needs to
        // adjust the state of the op to be the more general quad and aa types of the two ops and
        // then concatenate the per-quad data.
        fColorType = std::max(fColorType, that->fColorType);

        // The helper stores the aa type, but isCompatible(with true arg) allows the two ops' aa
        // types to be none and coverage, in which case this op's aa type must be lifted to coverage
        // so that quads with no aa edges can be batched with quads that have some/all edges aa'ed.
        if (upgradeToCoverageAAOnMerge) {
            fHelper.setAAType(GrAAType::kCoverage);
        }

        fQuads.concat(that->fQuads);
        return CombineResult::kMerged;
    }

    bool canAddQuads(int numQuads, GrAAType aaType) {
        // The new quad's aa type should be the same as the first quad's or none, except when the
        // first quad's aa type was already downgraded to none, in which case the stored type must
        // be lifted to back to the requested type.
        int quadCount = fQuads.count() + numQuads;
        if (aaType != fHelper.aaType() && aaType != GrAAType::kNone) {
            auto indexBufferOption = GrQuadPerEdgeAA::CalcIndexBufferOption(aaType, quadCount);
            if (quadCount > GrQuadPerEdgeAA::QuadLimit(indexBufferOption)) {
                // Promoting to the new aaType would've caused an overflow of the indexBuffer
                // limit
                return false;
            }

            // Original quad was downgraded to non-aa, lift back up to this quad's required type
            SkASSERT(fHelper.aaType() == GrAAType::kNone);
            fHelper.setAAType(aaType);
        } else {
            auto indexBufferOption = GrQuadPerEdgeAA::CalcIndexBufferOption(fHelper.aaType(),
                                                                            quadCount);
            if (quadCount > GrQuadPerEdgeAA::QuadLimit(indexBufferOption)) {
                return false; // This op can't grow any more
            }
        }

        return true;
    }

    // Similar to onCombineIfPossible, but adds a quad assuming its op would have been compatible.
    // But since it's avoiding the op list management, it must update the op's bounds.
    bool addQuad(DrawQuad* quad, const SkPMColor4f& color, GrAAType aaType) {
        SkRect newBounds = this->bounds();
        newBounds.joinPossiblyEmptyRect(quad->fDevice.bounds());

        DrawQuad extra;
        int count = quad->fEdgeFlags != GrQuadAAFlags::kNone ? GrQuadUtils::ClipToW0(quad, &extra)
                                                             : 1;
        if (count == 0 ) {
            // Just skip the append (trivial success)
            return true;
        } else if (!this->canAddQuads(count, aaType)) {
            // Not enough room in the index buffer for the AA type
            return false;
        } else {
            // Can actually add the 1 or 2 quads representing the draw
            fQuads.append(quad->fDevice, { color, quad->fEdgeFlags },
                          fHelper.isTrivial() ? nullptr : &quad->fLocal);
            if (count > 1) {
                fQuads.append(extra.fDevice, { color, extra.fEdgeFlags },
                              fHelper.isTrivial() ? nullptr : &extra.fLocal);
            }
            // Update the bounds
            this->setBounds(newBounds, HasAABloat(fHelper.aaType() == GrAAType::kCoverage),
                            IsHairline::kNo);
            return true;
        }
    }

    struct ColorAndAA {
        SkPMColor4f fColor;
        GrQuadAAFlags fAAFlags;
    };

    Helper fHelper;
    GrQuadBuffer<ColorAndAA> fQuads;
    char* fPrePreparedVertices = nullptr;

    GrSimpleMesh*  fMesh = nullptr;
    GrProgramInfo* fProgramInfo = nullptr;
    ColorType      fColorType;

    typedef GrMeshDrawOp INHERITED;
};

} // anonymous namespace

std::unique_ptr<GrDrawOp> GrFillRectOp::Make(GrRecordingContext* context,
                                             GrPaint&& paint,
                                             GrAAType aaType,
                                             DrawQuad* quad,
                                             const GrUserStencilSettings* stencil,
                                             InputFlags inputFlags) {
    return FillRectOp::Make(context, std::move(paint), aaType, std::move(quad), stencil,
                            inputFlags);
}

std::unique_ptr<GrDrawOp> GrFillRectOp::MakeNonAARect(GrRecordingContext* context,
                                                      GrPaint&& paint,
                                                      const SkMatrix& view,
                                                      const SkRect& rect,
                                                      const GrUserStencilSettings* stencil) {
    DrawQuad quad{GrQuad::MakeFromRect(rect, view), GrQuad(rect), GrQuadAAFlags::kNone};
    return FillRectOp::Make(context, std::move(paint), GrAAType::kNone, &quad, stencil,
                            InputFlags::kNone);
}

std::unique_ptr<GrDrawOp> GrFillRectOp::MakeOp(GrRecordingContext* context,
                                               GrPaint&& paint,
                                               GrAAType aaType,
                                               const SkMatrix& viewMatrix,
                                               const GrRenderTargetContext::QuadSetEntry quads[],
                                               int cnt,
                                               const GrUserStencilSettings* stencilSettings,
                                               int* numConsumed) {
    // First make a draw op for the first quad in the set
    SkASSERT(cnt > 0);

    DrawQuad quad{GrQuad::MakeFromRect(quads[0].fRect, viewMatrix),
                  GrQuad::MakeFromRect(quads[0].fRect, quads[0].fLocalMatrix),
                  quads[0].fAAFlags};
    paint.setColor4f(quads[0].fColor);
    std::unique_ptr<GrDrawOp> op = FillRectOp::Make(context, std::move(paint), aaType,
                                                    &quad, stencilSettings, InputFlags::kNone);
    FillRectOp* fillRects = op->cast<FillRectOp>();

    *numConsumed = 1;
    // Accumulate remaining quads similar to onCombineIfPossible() without creating an op
    for (int i = 1; i < cnt; ++i) {
        quad = {GrQuad::MakeFromRect(quads[i].fRect, viewMatrix),
                GrQuad::MakeFromRect(quads[i].fRect, quads[i].fLocalMatrix),
                quads[i].fAAFlags};

        GrAAType resolvedAA;
        GrQuadUtils::ResolveAAType(aaType, quads[i].fAAFlags, quad.fDevice,
                                   &resolvedAA, &quad.fEdgeFlags);

        if (!fillRects->addQuad(&quad, quads[i].fColor, resolvedAA)) {
            break;
        }

        (*numConsumed)++;
    }

    return op;
}

void GrFillRectOp::AddFillRectOps(GrRenderTargetContext* rtc,
                                  const GrClip& clip,
                                  GrRecordingContext* context,
                                  GrPaint&& paint,
                                  GrAAType aaType,
                                  const SkMatrix& viewMatrix,
                                  const GrRenderTargetContext::QuadSetEntry quads[],
                                  int cnt,
                                  const GrUserStencilSettings* stencilSettings) {

    int offset = 0;
    int numLeft = cnt;
    while (numLeft) {
        int numConsumed = 0;

        std::unique_ptr<GrDrawOp> op = MakeOp(context, GrPaint::Clone(paint), aaType, viewMatrix,
                                              &quads[offset], numLeft, stencilSettings,
                                              &numConsumed);

        offset += numConsumed;
        numLeft -= numConsumed;

        rtc->addDrawOp(clip, std::move(op));
    }

    SkASSERT(offset == cnt);
}

#if GR_TEST_UTILS

uint32_t GrFillRectOp::ClassID() {
    return FillRectOp::ClassID();
}

#include "src/gpu/GrDrawOpTest.h"
#include "src/gpu/SkGr.h"

GR_DRAW_OP_TEST_DEFINE(FillRectOp) {
    SkMatrix viewMatrix = GrTest::TestMatrixInvertible(random);
    SkRect rect = GrTest::TestRect(random);

    GrAAType aaType = GrAAType::kNone;
    if (random->nextBool()) {
        aaType = (numSamples > 1) ? GrAAType::kMSAA : GrAAType::kCoverage;
    }
    const GrUserStencilSettings* stencil = random->nextBool() ? nullptr
                                                              : GrGetRandomStencil(random, context);

    GrQuadAAFlags aaFlags = GrQuadAAFlags::kNone;
    aaFlags |= random->nextBool() ? GrQuadAAFlags::kLeft : GrQuadAAFlags::kNone;
    aaFlags |= random->nextBool() ? GrQuadAAFlags::kTop : GrQuadAAFlags::kNone;
    aaFlags |= random->nextBool() ? GrQuadAAFlags::kRight : GrQuadAAFlags::kNone;
    aaFlags |= random->nextBool() ? GrQuadAAFlags::kBottom : GrQuadAAFlags::kNone;

    if (random->nextBool()) {
        if (random->nextBool()) {
            // Single local matrix
            SkMatrix localMatrix = GrTest::TestMatrixInvertible(random);
            DrawQuad quad = {GrQuad::MakeFromRect(rect, viewMatrix),
                             GrQuad::MakeFromRect(rect, localMatrix), aaFlags};
            return GrFillRectOp::Make(context, std::move(paint), aaType, &quad, stencil);
        } else {
            // Pass local rect directly
            SkRect localRect = GrTest::TestRect(random);
            DrawQuad quad = {GrQuad::MakeFromRect(rect, viewMatrix),
                             GrQuad(localRect), aaFlags};
            return GrFillRectOp::Make(context, std::move(paint), aaType, &quad, stencil);
        }
    } else {
        // The simplest constructor
        DrawQuad quad = {GrQuad::MakeFromRect(rect, viewMatrix), GrQuad(rect), aaFlags};
        return GrFillRectOp::Make(context, std::move(paint), aaType, &quad, stencil);
    }
}

#endif
