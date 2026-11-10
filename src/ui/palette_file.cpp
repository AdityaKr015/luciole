#include "ui/palette_file.h"

#include "fs/xdg_dirs.h"
#include "render/core/color.h"

#include <array>
#include <format>
#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>
#include <utility>

namespace {
  constexpr std::array<std::pair<ColorRole, Color Palette::*>, 28> kSlots{{
      {ColorRole::Primary, &Palette::primary},
      {ColorRole::OnPrimary, &Palette::onPrimary},
      {ColorRole::PrimaryContainer, &Palette::primaryContainer},
      {ColorRole::OnPrimaryContainer, &Palette::onPrimaryContainer},
      {ColorRole::Secondary, &Palette::secondary},
      {ColorRole::OnSecondary, &Palette::onSecondary},
      {ColorRole::SecondaryContainer, &Palette::secondaryContainer},
      {ColorRole::OnSecondaryContainer, &Palette::onSecondaryContainer},
      {ColorRole::Tertiary, &Palette::tertiary},
      {ColorRole::OnTertiary, &Palette::onTertiary},
      {ColorRole::TertiaryContainer, &Palette::tertiaryContainer},
      {ColorRole::OnTertiaryContainer, &Palette::onTertiaryContainer},
      {ColorRole::Error, &Palette::error},
      {ColorRole::OnError, &Palette::onError},
      {ColorRole::ErrorContainer, &Palette::errorContainer},
      {ColorRole::OnErrorContainer, &Palette::onErrorContainer},
      {ColorRole::Surface, &Palette::surface},
      {ColorRole::OnSurface, &Palette::onSurface},
      {ColorRole::OnSurfaceVariant, &Palette::onSurfaceVariant},
      {ColorRole::SurfaceContainerLow, &Palette::surfaceContainerLow},
      {ColorRole::SurfaceContainer, &Palette::surfaceContainer},
      {ColorRole::SurfaceContainerHigh, &Palette::surfaceContainerHigh},
      {ColorRole::SurfaceContainerHighest, &Palette::surfaceContainerHighest},
      {ColorRole::OutlineVariant, &Palette::outlineVariant},
      {ColorRole::Shadow, &Palette::shadow},
      {ColorRole::Scrim, &Palette::scrim},
      {ColorRole::Hover, &Palette::hover},
      {ColorRole::OnHover, &Palette::onHover},
  }};

  Color& slot(Palette& palette, ColorRole role) {
    for (const auto& [candidate, member] : kSlots) {
      if (candidate == role) {
        return palette.*member;
      }
    }
    std::unreachable();
  }
} // namespace

Palette luciole::defaultPalette() {
  return {
      .primary = hex("#fff59b"),
      .onPrimary = hex("#0e0e43"),
      .primaryContainer = hex("#383526"),
      .onPrimaryContainer = hex("#fff59b"),
      .secondary = hex("#a9aefe"),
      .onSecondary = hex("#0e0e43"),
      .secondaryContainer = hex("#29294f"),
      .onSecondaryContainer = hex("#dfe0ff"),
      .tertiary = hex("#9bfece"),
      .onTertiary = hex("#0e0e43"),
      .tertiaryContainer = hex("#143a2b"),
      .onTertiaryContainer = hex("#9bfece"),
      .error = hex("#fd4663"),
      .onError = hex("#0e0e43"),
      .errorContainer = hex("#4f1521"),
      .onErrorContainer = hex("#ffd9df"),
      .surface = hex("#070722"),
      .onSurface = hex("#f3edf7"),
      .onSurfaceVariant = hex("#7c80b4"),
      .surfaceContainerLow = hex("#0b0b27"),
      .surfaceContainer = hex("#11112d"),
      .surfaceContainerHigh = hex("#1a1a39"),
      .surfaceContainerHighest = hex("#232345"),
      .outlineVariant = hex("#21215f"),
      .shadow = hex("#070722"),
      .scrim = hex("#000000"),
      .hover = hex("#9bfece"),
      .onHover = hex("#0e0e43"),
  };
}

std::filesystem::path luciole::palettePath() { return configHome() / "luciole" / "colors.toml"; }

std::expected<Palette, std::string>
luciole::parsePalette(std::string_view text, std::string_view source, const Palette& base) {
  toml::table table;
  try {
    table = toml::parse(text, source);
  } catch (const toml::parse_error& error) {
    return std::unexpected(
        std::format("{}:{}: {}", source, error.source().begin.line, std::string_view(error.description()))
    );
  }
  const auto* colors = table["colors"].as_table();
  if (colors == nullptr) {
    return std::unexpected(std::format("{}: missing [colors] table", source));
  }
  Palette result = base;
  for (const auto& [key, node] : *colors) {
    const auto role = colorRoleFromToken(key.str());
    if (!role) {
      continue; // Noctalia exposes more tokens than the toolkit uses.
    }
    const auto* value = node.as_string();
    Color color;
    if (value == nullptr || !tryParseHexColor(value->get(), color)) {
      return std::unexpected(std::format("{}: colors.{} must be a hex color such as \"#1a1a39\"", source, key.str()));
    }
    slot(result, *role) = color;
  }
  return result;
}

std::expected<Palette, std::string> luciole::loadPalette(const std::filesystem::path& path, const Palette& base) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error) {
      return base;
    }
    return std::unexpected(std::format("{}: cannot be read", path.string()));
  }
  std::ostringstream text;
  text << file.rdbuf();
  return parsePalette(text.str(), path.string(), base);
}
