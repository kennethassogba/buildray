import { readFile, readdir, stat } from 'node:fs/promises'
import { resolve, relative } from 'node:path'

const root = resolve('website/.vitepress/dist')
const base = new URL('https://kennethassogba.github.io/buildray/')
async function walk(dir) {
  const files = await readdir(dir, { withFileTypes: true })
  return (await Promise.all(files.map(f => f.isDirectory() ? walk(resolve(dir, f.name)) : resolve(dir, f.name)))).flat()
}
const html = (await walk(root)).filter(p => p.endsWith('.html'))
const pages = new Map(await Promise.all(html.map(async p => [p, await readFile(p, 'utf8')])))
const errors = new Set()
let checked = 0
for (const [file, source] of pages) {
  const page = new URL(relative(root, file), base)
  for (const match of source.matchAll(/\b(?:href|src)="([^"]+)"/g)) {
    const value = match[1].replaceAll('&amp;', '&')
    const url = new URL(value, page)
    if (url.origin !== base.origin || !['http:', 'https:'].includes(url.protocol)) continue
    checked++
    if (!url.pathname.startsWith(base.pathname)) {
      errors.add(`${relative(root, file)}: link escapes /buildray/: ${value}`)
      continue
    }
    let dest = resolve(root, decodeURIComponent(url.pathname.slice(base.pathname.length)))
    if (dest !== root && !dest.startsWith(root + '/')) throw new Error(`Invalid path: ${value}`)
    try {
      if ((await stat(dest)).isDirectory()) dest = resolve(dest, 'index.html')
      await stat(dest)
      if (url.hash && pages.has(dest)) {
        const id = decodeURIComponent(url.hash.slice(1))
        if (![...pages.get(dest).matchAll(/\bid="([^"]+)"/g)].some(m => m[1] === id)) {
          errors.add(`${relative(root, file)}: missing anchor ${value}`)
        }
      }
    } catch { errors.add(`${relative(root, file)}: missing file ${value}`) }
  }
}
for (const [name, lang] of [
  ...['index', 'start', 'concepts', 'optimize', 'measure', 'ci', 'results', 'next'].map(p => [`${p}.html`, 'en']),
  ...['index', 'start', 'concepts', 'optimize', 'measure', 'ci', 'results', 'next'].map(p => [`fr/${p}.html`, 'fr'])
]) {
  const text = pages.get(resolve(root, name))
  if (!text?.includes(`<html lang="${lang}"`)) errors.add(`Missing page or wrong language: ${name}`)
  if (!text?.includes('VPDoc')) errors.add(`Missing document layout: ${name}`)
}
if (errors.size) {
  console.error([...errors].join('\n'))
  process.exitCode = 1
} else console.log(`Checked ${checked} local links and assets across ${html.length} HTML pages; French and English guides present.`)
