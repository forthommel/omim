#include "choose_position_mark.hpp"
#include "compass.hpp"
#include "copyright_label.hpp"
#include "debug_label.hpp"
#include "drape_gui.hpp"
#include "gui_text.hpp"
#include "layer_render.hpp"
#include "ruler.hpp"
#include "ruler_helper.hpp"

#include "drape_frontend/visual_params.hpp"

#include "drape/batcher.hpp"
#include "drape/render_bucket.hpp"

#include "geometry/mercator.hpp"

#include "base/stl_add.hpp"

#include "std/bind.hpp"

namespace gui
{

LayerRenderer::~LayerRenderer()
{
  DestroyRenderers();
}

void LayerRenderer::Build(ref_ptr<dp::GpuProgramManager> mng)
{
  for (TRenderers::value_type & r : m_renderers)
    r.second->Build(mng);
}

void LayerRenderer::Render(ref_ptr<dp::GpuProgramManager> mng, bool routingActive,
                           ScreenBase const & screen)
{
  if (HasWidget(gui::WIDGET_RULER))
  {
    DrapeGui::GetRulerHelper().ResetTextDirtyFlag();
    DrapeGui::GetRulerHelper().Update(screen);
  }

  for (TRenderers::value_type & r : m_renderers)
  {
    if (routingActive && (r.first == gui::WIDGET_COMPASS || r.first == gui::WIDGET_RULER))
      continue;

    r.second->Render(screen, mng);
  }
}

void LayerRenderer::Merge(ref_ptr<LayerRenderer> other)
{
  bool activeOverlayFound = false;
  for (TRenderers::value_type & r : other->m_renderers)
  {
    TRenderers::iterator it = m_renderers.find(r.first);
    if (it != m_renderers.end())
    {
      auto newActiveOverlay = r.second->FindHandle(m_activeOverlayId);
      bool const updateActive = (m_activeOverlay != nullptr && newActiveOverlay != nullptr);
      it->second = move(r.second);
      if (!activeOverlayFound && updateActive)
      {
        activeOverlayFound = true;
        m_activeOverlay = newActiveOverlay;
        if (m_activeOverlay != nullptr)
          m_activeOverlay->OnTapBegin();
      }
    }
    else
    {
      m_renderers.insert(make_pair(r.first, move(r.second)));
    }
  }

  other->m_renderers.clear();
}

void LayerRenderer::SetLayout(TWidgetsLayoutInfo const & info)
{
  for (auto node : info)
  {
    auto renderer = m_renderers.find(node.first);
    if (renderer != m_renderers.end())
      renderer->second->SetPivot(node.second);
  }
}

void LayerRenderer::DestroyRenderers()
{
  m_renderers.clear();
}

void LayerRenderer::AddShapeRenderer(EWidget widget, drape_ptr<ShapeRenderer> && shape)
{
  if (shape == nullptr)
    return;

  VERIFY(m_renderers.insert(make_pair(widget, move(shape))).second, ());
}

bool LayerRenderer::OnTouchDown(m2::RectD const & touchArea)
{
  for (TRenderers::value_type & r : m_renderers)
  {
    m_activeOverlay = r.second->ProcessTapEvent(touchArea);
    if (m_activeOverlay != nullptr)
    {
      m_activeOverlayId = m_activeOverlay->GetOverlayID().m_featureId;
      m_activeOverlay->OnTapBegin();
      return true;
    }
  }

  return false;
}

void LayerRenderer::OnTouchUp(m2::RectD const & touchArea)
{
  if (m_activeOverlay != nullptr)
  {
    if (m_activeOverlay->IsTapped(touchArea))
      m_activeOverlay->OnTap();

    m_activeOverlay->OnTapEnd();
    m_activeOverlay = nullptr;
    m_activeOverlayId = FeatureID();
  }
}

void LayerRenderer::OnTouchCancel(m2::RectD const & touchArea)
{
  UNUSED_VALUE(touchArea);
  if (m_activeOverlay != nullptr)
  {
    m_activeOverlay->OnTapEnd();
    m_activeOverlay = nullptr;
    m_activeOverlayId = FeatureID();
  }
}

bool LayerRenderer::HasWidget(EWidget widget) const
{
  return m_renderers.find(widget) != m_renderers.end();
}

namespace
{

class ScaleLabelHandle : public MutableLabelHandle
{
  using TBase = MutableLabelHandle;
public:
  ScaleLabelHandle(uint32_t id, ref_ptr<dp::TextureManager> textures)
    : TBase(id, dp::LeftBottom, m2::PointF::Zero(), textures)
    , m_scale(0)
  {
    SetIsVisible(true);
  }

  bool Update(ScreenBase const & screen) override
  {
    int newScale = df::GetDrawTileScale(screen);
    if (m_scale != newScale)
    {
      m_scale = newScale;
      SetContent("Scale : " + strings::to_string(m_scale));
    }

    float vs = df::VisualParams::Instance().GetVisualScale();
    m2::PointF offset(10.0f * vs, 30.0f * vs);

    SetPivot(glsl::ToVec2(m2::PointF(screen.PixelRect().LeftBottom()) + offset));
    return TBase::Update(screen);
  }

private:
  int m_scale;
};

} // namespace

drape_ptr<LayerRenderer> LayerCacher::RecacheWidgets(TWidgetsInitInfo const & initInfo, ref_ptr<dp::TextureManager> textures)
{
  using TCacheShape = function<m2::PointF (Position anchor, ref_ptr<LayerRenderer> renderer, ref_ptr<dp::TextureManager> textures)>;
  static map<EWidget, TCacheShape> cacheFunctions
  {
    make_pair(WIDGET_COMPASS, bind(&LayerCacher::CacheCompass, this, _1, _2, _3)),
    make_pair(WIDGET_RULER, bind(&LayerCacher::CacheRuler, this, _1, _2, _3)),
    make_pair(WIDGET_COPYRIGHT, bind(&LayerCacher::CacheCopyright, this, _1, _2, _3)),
    make_pair(WIDGET_SCALE_LABEL, bind(&LayerCacher::CacheScaleLabel, this, _1, _2, _3))
  };

  drape_ptr<LayerRenderer> renderer = make_unique_dp<LayerRenderer>();
  for (auto node : initInfo)
  {
    auto cacheFunction = cacheFunctions.find(node.first);
    if (cacheFunction != cacheFunctions.end())
      cacheFunction->second(node.second, make_ref(renderer), textures);
  }

  // Flush gui geometry.
  GLFunctions::glFlush();

  return renderer;
}

drape_ptr<LayerRenderer> LayerCacher::RecacheChoosePositionMark(ref_ptr<dp::TextureManager> textures)
{
  m2::PointF const surfSize = DrapeGui::Instance().GetSurfaceSize();
  drape_ptr<LayerRenderer> renderer = make_unique_dp<LayerRenderer>();

  ChoosePositionMark positionMark = ChoosePositionMark(Position(surfSize * 0.5f, dp::Center));
  renderer->AddShapeRenderer(WIDGET_CHOOSE_POSITION_MARK, positionMark.Draw(textures));

  // Flush gui geometry.
  GLFunctions::glFlush();

  return renderer;
}

#ifdef RENRER_DEBUG_INFO_LABELS
drape_ptr<LayerRenderer> LayerCacher::RecacheDebugLabels(ref_ptr<dp::TextureManager> textures)
{
  drape_ptr<LayerRenderer> renderer = make_unique_dp<LayerRenderer>();

  float const vs = df::VisualParams::Instance().GetVisualScale();
  DebugInfoLabels debugLabels = DebugInfoLabels(Position(m2::PointF(10.0f * vs, 50.0f * vs), dp::Center));

  debugLabels.AddLabel(textures, "visible: km2, readed: km2, ratio:",
                       [](ScreenBase const & screen, string & content) -> bool
  {
    double const sizeX = screen.PixelRectIn3d().SizeX();
    double const sizeY = screen.PixelRectIn3d().SizeY();

    m2::PointD const p0 = screen.PtoG(screen.P3dtoP(m2::PointD(0.0, 0.0)));
    m2::PointD const p1 = screen.PtoG(screen.P3dtoP(m2::PointD(0.0, sizeY)));
    m2::PointD const p2 = screen.PtoG(screen.P3dtoP(m2::PointD(sizeX, sizeY)));
    m2::PointD const p3 = screen.PtoG(screen.P3dtoP(m2::PointD(sizeX, 0.0)));

    double const areaG = MercatorBounds::AreaOnEarth(p0, p1, p2) +
        MercatorBounds::AreaOnEarth(p2, p3, p0);

    double const sizeX_2d = screen.PixelRect().SizeX();
    double const sizeY_2d = screen.PixelRect().SizeY();

    m2::PointD const p0_2d = screen.PtoG(m2::PointD(0.0, 0.0));
    m2::PointD const p1_2d = screen.PtoG(m2::PointD(0.0, sizeY_2d));
    m2::PointD const p2_2d = screen.PtoG(m2::PointD(sizeX_2d, sizeY_2d));
    m2::PointD const p3_2d = screen.PtoG(m2::PointD(sizeX_2d, 0.0));

    double const areaGTotal = MercatorBounds::AreaOnEarth(p0_2d, p1_2d, p2_2d) +
        MercatorBounds::AreaOnEarth(p2_2d, p3_2d, p0_2d);

    ostringstream out;
    out << fixed << setprecision(2)
        << "visible: " << areaG / 1000000.0 << " km2"
        << ", readed: " << areaGTotal / 1000000.0 << " km2"
        << ", ratio: " << areaGTotal / areaG;
    content.assign(out.str());
    return true;
  });

  debugLabels.AddLabel(textures, "scale2d: m/px, scale2d * vs: m/px",
                       [](ScreenBase const & screen, string & content) -> bool
  {
    double const distanceG = MercatorBounds::DistanceOnEarth(screen.PtoG(screen.PixelRect().LeftBottom()),
                                                            screen.PtoG(screen.PixelRect().RightBottom()));

    double const vs = df::VisualParams::Instance().GetVisualScale();
    double const scale = distanceG / screen.PixelRect().SizeX();

    ostringstream out;
    out << fixed << setprecision(2)
        << "scale2d: " << scale << " m/px"
        << ", scale2d * vs: " << scale * vs << " m/px";
    content.assign(out.str());
    return true;
  });

  debugLabels.AddLabel(textures, "distance: m",
                       [](ScreenBase const & screen, string & content) -> bool
  {
    double const sizeX = screen.PixelRectIn3d().SizeX();
    double const sizeY = screen.PixelRectIn3d().SizeY();

    double const distance = MercatorBounds::DistanceOnEarth(screen.PtoG(screen.P3dtoP(m2::PointD(sizeX / 2.0, 0.0))),
                                                            screen.PtoG(screen.P3dtoP(m2::PointD(sizeX / 2.0, sizeY))));

    ostringstream out;
    out << fixed << setprecision(2)
        << "distance: " << distance << " m";
    content.assign(out.str());
    return true;
  });

  debugLabels.AddLabel(textures, "angle: ",
                       [](ScreenBase const & screen, string & content) -> bool
  {
    ostringstream out;
    out << fixed << setprecision(2)
        << "angle: " << screen.GetRotationAngle() * 180.0 / math::pi;
    content.assign(out.str());
    return true;
  });

  renderer->AddShapeRenderer(WIDGET_DEBUG_INFO, debugLabels.Draw(textures));

  // Flush gui geometry.
  GLFunctions::glFlush();

  return renderer;
}
#endif

m2::PointF LayerCacher::CacheCompass(Position const & position, ref_ptr<LayerRenderer> renderer,
                                     ref_ptr<dp::TextureManager> textures)
{
  m2::PointF compassSize;
  Compass compass = Compass(position);
  drape_ptr<ShapeRenderer> shape = compass.Draw(compassSize, textures, bind(&DrapeGui::CallOnCompassTappedHandler,
                                                                            &DrapeGui::Instance()));

  renderer->AddShapeRenderer(WIDGET_COMPASS, move(shape));

  return compassSize;
}

m2::PointF LayerCacher::CacheRuler(Position const & position, ref_ptr<LayerRenderer> renderer,
                                   ref_ptr<dp::TextureManager> textures)
{
  m2::PointF rulerSize;
  renderer->AddShapeRenderer(WIDGET_RULER,
                             Ruler(position).Draw(rulerSize, textures));
  return rulerSize;
}

m2::PointF LayerCacher::CacheCopyright(Position const & position, ref_ptr<LayerRenderer> renderer,
                                       ref_ptr<dp::TextureManager> textures)
{
  m2::PointF size;
  renderer->AddShapeRenderer(WIDGET_COPYRIGHT,
                             CopyrightLabel(position).Draw(size, textures));

  return size;
}

m2::PointF LayerCacher::CacheScaleLabel(Position const & position, ref_ptr<LayerRenderer> renderer, ref_ptr<dp::TextureManager> textures)
{
  MutableLabelDrawer::Params params;
  params.m_alphabet = "Scale: 1234567890";
  params.m_maxLength = 10;
  params.m_anchor = position.m_anchor;
  params.m_font = DrapeGui::GetGuiTextFont();
  params.m_pivot = position.m_pixelPivot;
  params.m_handleCreator = [textures](dp::Anchor, m2::PointF const &)
  {
    return make_unique_dp<ScaleLabelHandle>(EGuiHandle::GuiHandleScaleLabel, textures);
  };

  drape_ptr<ShapeRenderer> scaleRenderer = make_unique_dp<ShapeRenderer>();
  m2::PointF size = MutableLabelDrawer::Draw(params, textures, bind(&ShapeRenderer::AddShape, scaleRenderer.get(), _1, _2));

  renderer->AddShapeRenderer(WIDGET_SCALE_LABEL, move(scaleRenderer));
  return size;
}

} // namespace gui
