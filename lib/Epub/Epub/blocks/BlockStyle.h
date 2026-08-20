#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/css/CssStyle.h"

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  // Upper bound (in em) for an em-based horizontal margin or padding.  Percentage
  // insets use a viewport-relative cap below so real book layouts such as poems
  // can retain their intended position without letting malformed CSS collapse a
  // line to one or two words.
  static constexpr float MAX_HORIZONTAL_INSET_EM = 2.0f;

  CssTextAlign alignment = CssTextAlign::Justify;

  // Spacing (in pixels)
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;     // treated same as margin for rendering
  int16_t paddingBottom = 0;  // treated same as margin for rendering
  int16_t paddingLeft = 0;    // treated same as margin for rendering
  int16_t paddingRight = 0;   // treated same as margin for rendering
  int16_t textIndent = 0;
  bool textIndentDefined = false;  // true if text-indent was explicitly set in CSS
  bool textAlignDefined = false;   // true if text-align was explicitly set in CSS
  bool isRtl = false;              // true if resolved direction is RTL
  bool directionDefined = false;   // true if direction was explicitly set in CSS/HTML

  // Paragraph spacing=None deliberately clears the live vertical box values
  // of <p> elements in ChapterHtmlSlimParser. Converted FB2 and many EPUBs,
  // however, also encode semantic title/subtitle/author/date blocks as <p>
  // with explicit center/right alignment. Keep a tiny copy of their publisher
  // vertical spacing here so withoutBottom()/addBottom() can restore it after
  // that compact-prose override. Ordinary left/justified body paragraphs do
  // not opt in and therefore remain gap-free.
  bool preserveStructuralVerticalSpacing = false;
  int16_t structuralMarginTop = 0;
  int16_t structuralMarginBottom = 0;
  int16_t structuralPaddingTop = 0;
  int16_t structuralPaddingBottom = 0;

  // Set when this block was created by a <br> element. Used by startNewTextBlock to inject
  // a full line-height gap when the <br> block stays empty (section-break use case).
  // NOT propagated through getCombinedBlockStyle so it can't leak into sibling blocks.
  bool fromBrElement = false;

  // Combined insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }
  [[nodiscard]] int16_t topInset() const { return marginTop + paddingTop; }
  [[nodiscard]] int16_t bottomInset() const { return marginBottom + paddingBottom; }

  // Return a copy with bottom margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutBottom() const {
    BlockStyle result = *this;
    // If Paragraph spacing=None cleared this semantic paragraph's top box,
    // restore the publisher spacing before the block is laid out. This check
    // is intentionally pair-wise: an explicitly non-zero live value means no
    // compact override happened and must be left untouched.
    if (result.preserveStructuralVerticalSpacing && result.marginTop == 0 && result.paddingTop == 0) {
      result.marginTop = result.structuralMarginTop;
      result.paddingTop = result.structuralPaddingTop;
    }
    result.marginBottom = 0;
    result.paddingBottom = 0;
    return result;
  }

  // Return a copy with bottom margins/padding collapsed (max) with the source's.
  // Uses CSS margin collapsing: adjacent parent-child margins resolve to the larger value.
  [[nodiscard]] BlockStyle addBottom(const BlockStyle& source) const {
    BlockStyle result = *this;
    int16_t sourceMarginBottom = source.marginBottom;
    int16_t sourcePaddingBottom = source.paddingBottom;
    if (source.preserveStructuralVerticalSpacing && sourceMarginBottom == 0 && sourcePaddingBottom == 0) {
      sourceMarginBottom = source.structuralMarginBottom;
      sourcePaddingBottom = source.structuralPaddingBottom;
    }
    result.marginBottom = std::max(marginBottom, sourceMarginBottom);
    result.paddingBottom = static_cast<int16_t>(paddingBottom + sourcePaddingBottom);
    return result;
  }

  enum class CombineAxis : uint8_t {
    Horizontal = 1,  // margins left/right, padding left/right, text-align, text-indent
    Vertical = 2,    // margins top/bottom, padding top/bottom
  };

  // Combine this style's properties with a child style along the specified axis.
  // Properties on the other axis are kept from the child unchanged.
  [[nodiscard]] BlockStyle getCombinedBlockStyle(const BlockStyle& child, CombineAxis axis) const {
    BlockStyle result = child;

    if (axis == CombineAxis::Horizontal) {
      result.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
      result.marginRight = static_cast<int16_t>(child.marginRight + marginRight);
      result.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
      result.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
      if (!child.textIndentDefined && textIndentDefined) {
        result.textIndent = textIndent;
        result.textIndentDefined = true;
      }
      if (!child.textAlignDefined && textAlignDefined) {
        result.alignment = alignment;
        result.textAlignDefined = true;
      }
    } else {
      result.marginTop = std::max(child.marginTop, marginTop);
      result.marginBottom = std::max(child.marginBottom, marginBottom);
      result.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
      result.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
    }
    // fromBrElement is consumed by startNewTextBlock and should not leak through ancestor style merging.
    result.fromBrElement = false;

    // Direction is inherited independently of the horizontal/vertical box model.
    if (!child.directionDefined && directionDefined) {
      result.isRtl = isRtl;
      result.directionDefined = true;
    }
    return result;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0) {
    BlockStyle blockStyle;
    const float vw = viewportWidth;
    const auto maxEmHorizontalInsetPx = static_cast<int16_t>(emSize * MAX_HORIZONTAL_INSET_EM);
    const auto maxPercentHorizontalInsetPx = static_cast<int16_t>(viewportWidth * 2 / 5);
    const auto resolveHorizontalInset = [&](const CssLength& length) {
      const auto resolved = length.toPixelsInt16(emSize, vw);
      const auto maximum = length.unit == CssUnit::Percent ? maxPercentHorizontalInsetPx : maxEmHorizontalInsetPx;
      return std::clamp<int16_t>(resolved, 0, maximum);
    };
    // Resolve all CssLength values to pixels using the current font's em size and viewport width
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, vw);
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, vw);
    blockStyle.marginLeft = resolveHorizontalInset(cssStyle.marginLeft);
    blockStyle.marginRight = resolveHorizontalInset(cssStyle.marginRight);

    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, vw);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, vw);
    blockStyle.paddingLeft = resolveHorizontalInset(cssStyle.paddingLeft);
    blockStyle.paddingRight = resolveHorizontalInset(cssStyle.paddingRight);

    // An explicit centered/right-aligned paragraph is commonly structural
    // typography (FB2 title/subtitle/text-author/date) rather than body prose.
    // Preserve only those cases; left/justify body paragraphs stay compact.
    blockStyle.preserveStructuralVerticalSpacing =
        cssStyle.hasTextAlign() &&
        (cssStyle.textAlign == CssTextAlign::Center || cssStyle.textAlign == CssTextAlign::Right);
    if (blockStyle.preserveStructuralVerticalSpacing) {
      blockStyle.structuralMarginTop = blockStyle.marginTop;
      blockStyle.structuralMarginBottom = blockStyle.marginBottom;
      blockStyle.structuralPaddingTop = blockStyle.paddingTop;
      blockStyle.structuralPaddingBottom = blockStyle.paddingBottom;
    }

    // For textIndent: if it's a percentage we can't resolve (no viewport width),
    // leave textIndentDefined=false so the space-width fallback in resolveFirstLineIndent() is used
    if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
      blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
      blockStyle.textIndentDefined = true;
    }
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    // User setting overrides CSS, unless "Book's Style" alignment setting is selected
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    if (cssStyle.hasDirection()) {
      blockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
      blockStyle.directionDefined = true;
    }
    return blockStyle;
  }
};
