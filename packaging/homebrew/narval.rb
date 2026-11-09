# Homebrew formula for narval. Lives in the tap repository
# git.giorgio-nas.fr/arthur/homebrew-narval as Formula/narval.rb:
#
#   brew tap arthur/narval https://git.giorgio-nas.fr/arthur/homebrew-narval.git
#   brew install narval
#
# packaging/render.sh VERSION fills in url and sha256 for a release.
class Narval < Formula
  desc "Terminal text editor configured in Lisp, with LSP and a debugger"
  homepage "https://git.giorgio-nas.fr/arthur/narval"
  url "https://git.giorgio-nas.fr/arthur/narval/releases/download/v@VERSION@/narval-@VERSION@.tar.gz"
  sha256 "@SHA256_SOURCE@"
  license "MIT"
  head "https://git.giorgio-nas.fr/arthur/narval.git", branch: "main"

  depends_on "meson" => :build
  depends_on "ninja" => :build

  def install
    system "meson", "setup", "build", *std_meson_args
    system "meson", "compile", "-C", "build", "--verbose"
    system "meson", "install", "-C", "build"
  end

  def caveats
    <<~EOS
      The first start sets up ~/.narval: the default config and
      ~/.narval/packages/personal-config/init.lisp for your own settings.
      The bundled packages stay in #{opt_pkgshare}/packages and upgrade
      with narval.
    EOS
  end

  test do
    assert_match "Narval", shell_output("#{bin}/narval --version")
    assert_predicate pkgshare/"config/init.lisp", :exist?
    ENV["HOME"] = testpath
    system bin/"narval", "--sync-packages"
    assert_predicate testpath/".narval/config/init.lisp", :exist?
  end
end
