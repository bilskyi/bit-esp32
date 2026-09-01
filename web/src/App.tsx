// Placeholder so the bundle mount is testable before the real UI exists.
// Task 2 replaces this with the actual playground.
function App() {
  return (
    <main
      style={{
        display: 'flex',
        minHeight: '100vh',
        alignItems: 'center',
        justifyContent: 'center',
      }}
    >
      <section
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--rule)',
          borderRadius: 4,
          boxShadow: 'var(--shadow)',
          padding: '2rem 2.5rem',
          textAlign: 'center',
        }}
      >
        <h1 style={{ margin: '0 0 0.75rem', fontSize: '1.5rem' }}>Voice companion</h1>
        <p
          style={{
            margin: 0,
            letterSpacing: '0.16em',
            textTransform: 'uppercase',
            fontSize: '0.8rem',
            color: 'var(--ink-3)',
          }}
        >
          playground
        </p>
      </section>
    </main>
  )
}

export default App
